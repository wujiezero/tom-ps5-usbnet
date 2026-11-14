/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tom-ps5-usbnet: a file manager for a PS5, served over a USB 10 GbE or 2.5 GbE adapter
 * that has its own address on the LAN, or, without one it can drive, over the console's
 * own network on port TILE_PORT. Runs only when the console's owner sends it to their
 * ELF loader.
 *
 * With an adapter whose network is not there (no cable, or no address), the page goes
 * on the console's own network too, until the adapter's network is back.
 *
 * The page has no login: whoever reaches the port on the LAN can read and delete files
 * on the console. See files.c. The run ends from the page; with an adapter, also when
 * the adapter goes away or the console sleeps.
 *
 * MAC addresses have to be in the frames and are never logged. */
#include "adapter.h"
#include "crash.h"
#include "files.h"
#include "forward.h"
#include "local_http.h"
#include "log.h"
#include "net.h"
#include "secret.h"
#include "smb.h"
#include "smb_client.h"
#include "tile.h"
#include "vendor.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef LINK_WAIT_MS
#define LINK_WAIT_MS 15000L
#endif
#define ADDRESS_WAIT_MS 40000L
#define LINK_SETTLE_US 1000000
#define LINK_POLL_MS 1000L
#define LINK_READ_TRIES 3               /* status reads that fail in a row before the link counts as down */
#define LOCAL_RETRY_MS 5000L
#define FALLBACK_AFTER_MS 3000L         /* the adapter's network gone this long: the page moves */
#define RETURN_GRACE_MS 15000L          /* the console's own network keeps the page this long after */
#define RETURN_IDLE_POLLS 3             /* and until this many polls in a row find no connection */
#define NATIVE_CHECK_MS 10000L
#define CONFIG_BYTES 2048
#define NATIVE_TICK_US 250000
#define NATIVE_PORT_WAIT_S 30
#define SLEEP_JUMP_MS 5000L             /* the wall clock ran on while the monotonic one stood */

enum page_on { PAGE_UNDECIDED, PAGE_USB, PAGE_NATIVE };
enum local_on { LOCAL_NONE, LOCAL_LOOPBACK, LOCAL_EVERYWHERE };

static struct {
    struct net_lease lease;             /* the last address the adapter had */
    uint8_t mac[6];
    unsigned link_mbps;
    int stop, link_up;
    unsigned bulk_in, bulk_out;
    enum rtl_chip chip;
    int native;                         /* no adapter at all */
    char native_ip[16];
    /* With an adapter: where the page is, and the kernel's listener. */
    enum page_on page;
    enum local_on local;
    int usb_ok, elevated, forwarding, announce, local_failed, idle_polls;
    long lost_at, back_at, local_tried, native_checked;
} g;

/* What info() reads from the server's threads: the addresses and usb_ok. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void net_assert(const char *what, const char *file, int line)
{
    say("lwIP assertion %s at %s:%d", what, file, line);
    checkpoint();
    _exit(3);
}

static const char *chip_name(void)
{
    return g.native ? "" : g.chip == RTL_8156B ? "RTL8156B" : g.chip == RTL_8157 ? "RTL8157" : "RTL8159";
}

/* mode: "usb" on the adapter, "native" without one, "fallback" with one whose network is
 * not there. ip and port are where the page is now; usb_ip and native_ip let a page on one
 * find the other. */
static int info(void *context, char *out, size_t capacity)
{
    char usb_ip[16], native_ip[16];
    int usb;

    (void)context;
    pthread_mutex_lock(&g_lock);
    memcpy(usb_ip, g.lease.ip, sizeof usb_ip);
    memcpy(native_ip, g.native_ip, sizeof native_ip);
    usb = g.usb_ok;
    pthread_mutex_unlock(&g_lock);
    if (!g.native && !usb && g.link_up) {
        struct net_lease lease;         /* the address came before the main loop saw it */

        if (net_address(&lease) == 0) {
            usb = 1;
            memcpy(usb_ip, lease.ip, sizeof usb_ip);
        }
    }
    snprintf(out, capacity, "{\"name\":\"" APP_NAME "\",\"version\":\"" APP_VERSION "\",\"mode\":\"%s\",\"adapter\":\"%s\",\"ip\":\"%s\","
        "\"port\":%d,\"link_mbps\":%u,\"max_mbps\":%u,\"usb_ip\":\"%s\",\"native_ip\":\"%s\",\"native_port\":%d}",
        g.native ? "native" : usb ? "usb" : "fallback", chip_name(), usb ? usb_ip : native_ip, usb ? 80 : TILE_PORT,
        g.native || !g.link_up ? 0 : g.link_mbps, g.native ? 0 : vnd_regs_max_mbps(), g.native ? "" : usb_ip, native_ip, TILE_PORT);
    return 0;
}

static void stop(void *context)
{
    (void)context;
    __atomic_store_n(&g.stop, 1, __ATOMIC_RELAXED);
}

static void log_file_line(const char *line)
{
    say("%s", line);
    checkpoint();
}

static int route(void *context, const struct http_request *request, struct http_stream *stream)
{
    char json[128];
    int result;

    (void)context;
    if (!strcmp(request->path, "/tile/status")) {
        tile_status(json, sizeof json);
        return http_respond(stream, 200, "application/json", json);
    }
    if (!strcmp(request->path, "/tile/remove") && !strcmp(request->method, "POST")) {
        tile_remove();
        tile_status(json, sizeof json);
        return http_respond(stream, 200, "application/json", json);
    }
    if (!strcmp(request->path, "/tile/install") && !strcmp(request->method, "POST")) {
        tile_install();
        return http_respond(stream, 200, "application/json", "{}");
    }
    result = forward_route(request, stream);
    if (result != HTTP_NOT_MINE) return result;
    result = smb_route(request, stream);
    return result != HTTP_NOT_MINE ? result : files_route(request, stream);
}

#ifndef SMB_STORE
#define SMB_STORE APP_DIR "/nas.txt"
#endif
#ifndef FORWARD_STORE
#define FORWARD_STORE APP_DIR "/forward.txt"
#endif

static const struct http_backend g_backend = {.route = route, .info = info, .stop = stop};

/* There is no link notification in use: the MAC's status register is enough. A read
 * that fails is the chip not answering over USB, which is not the cable: it is said as
 * what it is, and only counts as no link once it lasts. */
static void watch_link(long t)
{
    static long last_poll;
    static int unreadable;
    uint16_t status = 0;
    unsigned mbps;
    int up;

    if (t - last_poll < LINK_POLL_MS) return;
    last_poll = t;
    if (vnd_regs_read_phy(&status) != 0) {
        if (!unreadable++) {
            say("link status unreadable, errno %d", errno);
            checkpoint();
        }
        if (unreadable < LINK_READ_TRIES) return;
        status = 0;
    } else if (unreadable) {
        say("link status readable again after %d failed reads", unreadable);
        unreadable = 0;
    }
    up = rtl_phy_link(status);
    mbps = rtl_phy_mbps(status);
    if (up == g.link_up) {
        if (up && mbps != g.link_mbps) {
            say("link now %u Mbit/s, was %u", mbps, g.link_mbps);
            g.link_mbps = mbps;
            checkpoint();
        }
        return;
    }
    g.link_up = up;
    if (up) {
        g.link_mbps = mbps;
        say("link up, %u Mbit/s", mbps);
        if (vnd_regs_enabled() == 0) say("the MAC is no longer enabled: the chip reset itself, replug the adapter");
    } else {
        say("link down");
    }
    vnd_regs_mark_link(up);
    net_link(up);
    checkpoint();
}

/* Waits for the chip's fastest rate, which a link that comes up slower can still reach
 * while negotiating. */
static int wait_for_link(void)
{
    long started = now_ms();
    int up = 0;

    while (now_ms() - started < LINK_WAIT_MS && !vnd_gone()) {
        uint16_t status = vnd_regs_phy_status();

        if (rtl_phy_link(status)) {
            up = 1;
            if (rtl_phy_mbps(status) >= vnd_regs_max_mbps()) break;
        }
        usleep(100000);
    }
    return up ? 0 : -1;
}

/* The console's own address, as its routing table would pick for the LAN. A UDP connect
 * sends nothing. Never logged. */
static void native_address(char *out, size_t capacity)
{
    struct sockaddr_in to, mine;
    socklen_t size = sizeof mine;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    out[0] = 0;
    if (fd < 0) return;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x08080808u);
    if (connect(fd, (struct sockaddr *)&to, sizeof to) == 0 && getsockname(fd, (struct sockaddr *)&mine, &size) == 0 &&
            mine.sin_addr.s_addr != htonl(INADDR_ANY))
        inet_ntop(AF_INET, &mine.sin_addr, out, (socklen_t)capacity);
    close(fd);
}

/* Returns whether it changed. */
static int refresh_native(void)
{
    char ip[sizeof g.native_ip];
    int changed;

    native_address(ip, sizeof ip);
    pthread_mutex_lock(&g_lock);
    changed = strcmp(ip, g.native_ip) != 0;
    memcpy(g.native_ip, ip, sizeof ip);
    pthread_mutex_unlock(&g_lock);
    return changed;
}

/* The page's address on the console's own network, said on the screen and in a
 * notification; `why` leads the notification. */
static void announce_native(const char *why)
{
    char text[160], ip[sizeof g.native_ip];

    pthread_mutex_lock(&g_lock);
    memcpy(ip, g.native_ip, sizeof ip);
    pthread_mutex_unlock(&g_lock);
    if (ip[0]) {
        console_line("console address: %s, http://%s:%d/", ip, ip, TILE_PORT);
        snprintf(text, sizeof text, "%shttp://%s:%d/", why, ip, TILE_PORT);
    } else {
        say("the console's address is not known");
        snprintf(text, sizeof text, "%s端口 %d：用主机的 IP 地址打开", why, TILE_PORT);
    }
    notify(text);
    checkpoint();
}

/* The kernel's listener: on the loopback address for the icon while the page is on the
 * adapter, on every address while it is not. Going back to the loopback waits, so that a
 * page open on the console's address can move to the adapter's, and what it is sending
 * can end. */
static void watch_local(long t)
{
    enum local_on want = g.page == PAGE_NATIVE ? LOCAL_EVERYWHERE : g.elevated ? LOCAL_LOOPBACK : LOCAL_NONE;

    if (g.local != LOCAL_NONE && local_http_broken()) {
        say("local page: the listener failed, starting it again");
        if (local_http_close() == 0) g.local = LOCAL_NONE;
    }
    if (g.local == LOCAL_EVERYWHERE && want != LOCAL_EVERYWHERE) {
        g.idle_polls = local_http_connections() ? 0 : g.idle_polls + 1;
        if (t - g.back_at < RETURN_GRACE_MS || g.idle_polls < RETURN_IDLE_POLLS) return;
    }
    if (want != g.local && (!g.local_failed || t - g.local_tried >= LOCAL_RETRY_MS)) {
        if (g.local != LOCAL_NONE) {
            if (local_http_close() != 0) return;
            g.local = LOCAL_NONE;
        }
        if (want != LOCAL_NONE) {
            g.local_tried = t;
            if (local_http_start(&g_backend, TILE_PORT, want == LOCAL_EVERYWHERE) == 0) {
                g.local = want;
                g.local_failed = 0;
                say("local page: listening on %s", want == LOCAL_EVERYWHERE ? "every address" : "the loopback address");
            } else if (!g.local_failed++) {
                say("local page: port %d is held, errno %d, trying again", TILE_PORT, errno);
            }
            checkpoint();
        }
    }
    if (g.announce && g.local == LOCAL_EVERYWHERE) {
        g.announce = 0;
        announce_native(g.link_up ? "未获取到 IP 地址，改用主机网络：" : "网线未连接，改用主机网络：");
    }
}

static void usb_back(const struct net_lease *lease, long t)
{
    static const struct vnd_settings serving = {8, 1, VND_TX_BYTES};
    static const struct vnd_settings serving_linux = {8, 1, VND_LINUX_TX_BYTES};
    char text[96];
    int first = !g.lease.ip[0], moved = strcmp(lease->ip, g.lease.ip) != 0, returning = g.page == PAGE_NATIVE;

    if (first) vnd_configure(g.chip == RTL_8159 ? &serving : &serving_linux);
    if (moved) say("address from %s%s", lease->source, first ? "" : ", a new one");
    if (returning) say("page back on the adapter");
    pthread_mutex_lock(&g_lock);
    g.lease = *lease;
    g.usb_ok = 1;
    pthread_mutex_unlock(&g_lock);
    g.page = PAGE_USB;
    g.back_at = t;
    g.idle_polls = 0;
    g.announce = 0;
    smb_client_use_adapter(1);
    if (moved && g.forwarding) {
        forward_stop();
        g.forwarding = 0;
    }
    if (!g.forwarding) {
        if (forward_start(lease->ip) == 0) g.forwarding = 1;
        else say("port forwarding did not start");
    }
    console_line("LAN address: %s, http://%s/", lease->ip, lease->ip);
    snprintf(text, sizeof text, "%shttp://%s/", returning ? "网卡已恢复，切回网卡：" : "", lease->ip);
    notify(text);
    checkpoint();
}

static void usb_lost(long t)
{
    if (g.page == PAGE_USB) say("the adapter's network is gone: %s", g.link_up ? "no address" : "no link");
    say("page on the console's own network until the adapter's is back");
    pthread_mutex_lock(&g_lock);
    g.usb_ok = 0;
    pthread_mutex_unlock(&g_lock);
    g.page = PAGE_NATIVE;
    g.announce = 1;
    smb_client_use_adapter(0);
    refresh_native();
    g.native_checked = t;
    checkpoint();
}

/* Where the page is: on the adapter while it has a link and an address, else, after a
 * moment for a link that only blinked, on the console's own network. Off the adapter the
 * address is looked for at every turn, so that the page is said to be back as soon as it
 * can be reached there. */
static void watch_page(long t)
{
    static long last_poll;
    struct net_lease lease;
    int usable;

    if (g.page == PAGE_USB && t - last_poll < LINK_POLL_MS) return;
    last_poll = t;
    usable = g.link_up && net_address(&lease) == 0;
    if (usable) {
        g.lost_at = 0;
        if (g.page != PAGE_USB || strcmp(lease.ip, g.lease.ip)) usb_back(&lease, t);
    } else if (g.page == PAGE_UNDECIDED) {
        usb_lost(t);
    } else if (g.page == PAGE_USB) {
        if (!g.lost_at) g.lost_at = t;
        if (t - g.lost_at >= FALLBACK_AFTER_MS) usb_lost(t);
    } else if (g.page == PAGE_NATIVE && t - g.native_checked >= NATIVE_CHECK_MS) {
        g.native_checked = t;
        if (refresh_native() && g.native_ip[0]) g.announce = 1;      /* only this thread writes it */
    }
    watch_local(t);
}

static int keep_going(void)
{
    long t = now_ms();

    watch_link(t);
    watch_page(t);
    return !__atomic_load_n(&g.stop, __ATOMIC_RELAXED) && !vnd_gone();
}

/* With a link, the address is waited for before the page goes anywhere. */
static void wait_for_address(void)
{
    struct net_lease lease;
    long started = now_ms();

    while (now_ms() - started < ADDRESS_WAIT_MS && !vnd_gone()) {
        watch_link(now_ms());
        if (!g.link_up || net_address(&lease) == 0) return;
        usleep(50000);
    }
    say("no address");
}

static void run_network(void)
{
    static const struct vnd_settings joining = {8, 1, 16 * 1024};
    /* One transfer at a time, eight receive transfers: on the console 3.2 Gbit/s up and
     * 3.0 down, where thirty-two gave 2.6 up and 3.2 down, and uploads are what this is
     * for. Two transfers at a time were slower, and were in use when an early build
     * powered the console off. The serving settings are taken with the first address
     * (usb_back), Linux's aggregate size for the RTL8156B and RTL8157: nothing larger has
     * been sent to them. */

    vnd_configure(&joining);
    if (wait_for_link() == 0) {
        g.link_up = 1;
        g.link_mbps = rtl_phy_mbps(vnd_regs_phy_status());
        vnd_regs_mark_link(1);
        net_link(1);
        usleep(LINK_SETTLE_US);
        say("link up, %u Mbit/s", g.link_mbps);
    } else {
        say("no Ethernet link");
    }
    if (vnd_regs_open_filter() != 0) {
        say("the packet filter could not be set");
        return;
    }
    if (net_up() != 0) {
        say("DHCP did not start");
        return;
    }
    files_set_log(log_file_line);
    smb_set_store(SMB_STORE);
    say("NAS password key %s", secret_ready() ? "ready" : "unavailable");
    smb_client_use_adapter(0);
    forward_set_store(FORWARD_STORE);
    if (g.link_up) wait_for_address();
    crash_stage("taking rights");
    g.elevated = tile_elevate() == 0;
    if (g.elevated) files_sweep();
    crash_stage("starting the page");
    watch_page(now_ms());
    if (g.elevated) tile_resume();
    crash_stage("serving");
    if (net_serve(&g_backend, keep_going) != 0) say("the web server did not start: errno %d", errno);
    else say("serving ended: %s", vnd_gone() ? "the adapter was lost" : "asked to stop");
    checkpoint();
    if (g.forwarding) forward_stop();
    files_stop();
    local_http_stop();
    net_stop();
}

/* The one configuration whose only interface is the vendor's. */
static int find_vendor(struct adapter *a)
{
    static uint8_t config[CONFIG_BYTES];
    int index;

    for (index = 0; index < 8; index++) {
        struct rtl_usb_interface iface;
        int len = adapter_read_config(a, index, config, sizeof config);

        if (len <= 0) continue;
        if (rtl_parse_config(config, (size_t)len, 0, 0, &iface) == 0 && iface.class_code == 0xff) {
            g.bulk_in = iface.bulk_in;
            g.bulk_out = iface.bulk_out;
            return index;
        }
    }
    return -1;
}

/* Which chip, from PLA_TCR0 as Linux reads it before choosing a configuration. 0 if
 * this is not one this program drives. */
static int identify(struct adapter *a, uint16_t *version)
{
    struct rtl_usb_device device = {0x0bda, a->product, 0, 0, 0};

    *version = vnd_regs_read_version(a->fd);
    if (rtl_identify_target(&device, *version, &g.chip) != 0) {
        say("chip version %#06x on %04x:%04x is not supported", *version, 0x0bda, a->product);
        return 0;
    }
    say("chip %s, version %#06x", chip_name(), *version);
    return 1;
}

static void run(struct adapter *a)
{
    static const struct vnd_hooks hooks = {net_frames};
    static const struct link_ops link = {vnd_tx_reserve, vnd_tx_commit};
    uint16_t version;
    int vendor;

    if (!identify(a, &version)) {
        a->unusable = 1;
        return;
    }
    vendor = find_vendor(a);
    if (vendor < 0) {
        say("this adapter has no vendor configuration");
        a->unusable = 1;
        return;
    }
    if (adapter_read_mac(a, g.mac) != 0) {
        say("the adapter's address could not be read");
        return;
    }
    if (adapter_select_config(a, vendor) != 0) return;
    if (a->ecm_index >= 0) adapter_pass_through_ecm(a);        /* failing here costs nothing */
    if (adapter_select_config(a, vendor) != 0) return;
    a->data_iface = 0;
    if (adapter_driver_active(a)) {
        say("a kernel driver has the adapter: stopping");
        return;
    }
    vnd_regs_attach(a->fd, g.chip, version);
    if (vnd_regs_bring_up(g.mac) != 0) {
        say("chip bring-up failed");
        notify("网卡初始化失败");
        return;
    }
    if (vnd_open(a->fd, g.chip, g.bulk_in, g.bulk_out, &hooks) == 0) {
        if (net_start(g.mac, &link) != 0) say("the network stack did not start: errno %d", errno);
        else if (vnd_start() == 0) run_network();
    }
    vnd_regs_stop();
    vnd_close();
}

/* ---- without an adapter --------------------------------------------------- */

static void announce(void)
{
    refresh_native();
    announce_native("");
}

static int listen_everywhere(void)
{
    int waited;

    for (waited = 0; waited < NATIVE_PORT_WAIT_S; waited++) {
        if (local_http_start(&g_backend, TILE_PORT, 1) == 0) return 0;
        if (!waited) say("port %d is held, errno %d, trying again", TILE_PORT, errno);
        sleep(1);
    }
    return -1;
}

static long wall_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Rest mode keeps this process but not its listener, and may bring a new address. On
 * the firmware ps5upload measured, the monotonic clock stops across it. */
static int run_native(void)
{
    long wall = wall_ms(), mono = now_ms();

    g.native = 1;
    say("serving on the console's own network");
    notify("未找到可用的 USB 网卡，使用主机网络");
    files_set_log(log_file_line);
    smb_set_store(SMB_STORE);
    say("NAS password key %s", secret_ready() ? "ready" : "unavailable");
    smb_client_use_adapter(0);
    forward_set_store(FORWARD_STORE);   /* edited here, used with an adapter */
    crash_stage("taking rights");
    if (tile_elevate() == 0) files_sweep();
    if (listen_everywhere() != 0) {
        say("port %d stayed held", TILE_PORT);
        notify("端口被占用，已退出");
        return 1;
    }
    announce();
    tile_resume();
    crash_stage("serving");
    while (!__atomic_load_n(&g.stop, __ATOMIC_RELAXED)) {
        long w, m;

        usleep(NATIVE_TICK_US);
        w = wall_ms();
        m = now_ms();
        if ((w - wall) - (m - mono) > SLEEP_JUMP_MS || local_http_broken()) {
            say("the listener is restarted: %s", local_http_broken() ? "it failed" : "the console slept");
            local_http_stop();
            if (listen_everywhere() != 0) {
                say("port %d stayed held", TILE_PORT);
                break;
            }
            announce();
        }
        wall = w;
        mono = m;
    }
    files_stop();
    local_http_stop();
    return 0;
}

int main(void)
{
    int result;

    signal(SIGPIPE, SIG_IGN);           /* the loader's socket may already be gone */
    if (log_open() != 0) console_line("no log file: errno %d", errno);
    crash_log_start();
    log_firmware();
    notify("正在启动");
    result = adapter_main(run);
    if (result == ADAPTER_NONE) result = run_native();
    say("stopped");
    checkpoint();
    notify(result || g.native ? "已退出" : adapter_gone ? "网卡失联，已退出，请重新插拔网卡" : "已退出，网卡已恢复");
    log_close();
    return result;
}
