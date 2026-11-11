/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tom-ps5-usbnet: a file manager for a PS5, served over a USB 10 GbE or 2.5 GbE adapter
 * that has its own address on the LAN, or, without one it can drive, over the console's
 * own network on port TILE_PORT. Runs only when the console's owner sends it to their
 * ELF loader.
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

#define LINK_WAIT_MS 15000L
#define ADDRESS_WAIT_MS 40000L
#define LINK_SETTLE_US 1000000
#define LINK_POLL_MS 1000L
#define LOCAL_RETRY_S 5
#define CONFIG_BYTES 2048
#define NATIVE_TICK_US 250000
#define NATIVE_PORT_WAIT_S 30
#define SLEEP_JUMP_MS 5000L             /* the wall clock ran on while the monotonic one stood */

static struct {
    struct net_lease lease;
    uint8_t mac[6];
    unsigned link_mbps;
    int stop, link_up, local_waiting, retry_stop;
    unsigned bulk_in, bulk_out;
    enum rtl_chip chip;
    int native;
    char native_ip[16];
} g;

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

static int info(void *context, char *out, size_t capacity)
{
    (void)context;
    snprintf(out, capacity, "{\"name\":\"" APP_NAME "\",\"version\":\"" APP_VERSION "\",\"mode\":\"%s\",\"adapter\":\"%s\",\"ip\":\"%s\","
        "\"port\":%d,\"link_mbps\":%u,\"max_mbps\":%u}", g.native ? "native" : "usb", chip_name(), g.native ? g.native_ip : g.lease.ip,
        g.native ? TILE_PORT : 80, g.native ? 0 : g.link_mbps, g.native ? 0 : vnd_regs_max_mbps());
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

/* There is no link notification in use: the MAC's status register is enough. */
static void watch_link(long t)
{
    static long last_poll;
    uint16_t status;
    int up;

    if (t - last_poll < LINK_POLL_MS) return;
    last_poll = t;
    status = vnd_regs_phy_status();
    up = rtl_phy_link(status);
    if (up) g.link_mbps = rtl_phy_mbps(status);
    if (up == g.link_up) return;
    g.link_up = up;
    say("link %s", up ? "up" : "down");
    vnd_regs_mark_link(up);
    net_link(up);
    checkpoint();
}

static int keep_going(void)
{
    watch_link(now_ms());
    return !__atomic_load_n(&g.stop, __ATOMIC_RELAXED) && !vnd_gone();
}

/* SO_REUSEADDR is set, so EADDRINUSE here is a listener: an earlier run that has not gone.
 * Only the icon needs this port. */
static void *local_retry(void *unused)
{
    (void)unused;
    while (!__atomic_load_n(&g.retry_stop, __ATOMIC_RELAXED)) {
        sleep(LOCAL_RETRY_S);
        if (!__atomic_load_n(&g.retry_stop, __ATOMIC_RELAXED) && local_http_start(&g_backend, TILE_PORT, 0) == 0) {
            say("local page: listening");
            checkpoint();
            break;
        }
    }
    __atomic_store_n(&g.local_waiting, 0, __ATOMIC_RELAXED);
    return NULL;
}

static void start_local_page(void)
{
    pthread_t thread;

    if (local_http_start(&g_backend, TILE_PORT, 0) == 0) return;
    say("local page: port %d is held, errno %d, trying again", TILE_PORT, errno);
    g.local_waiting = 1;
    if (pthread_create(&thread, NULL, local_retry, NULL) == 0) pthread_detach(thread);
    else g.local_waiting = 0;
}

static void stop_local_page(void)
{
    int tick;

    __atomic_store_n(&g.retry_stop, 1, __ATOMIC_RELAXED);
    for (tick = 0; tick < 700 && __atomic_load_n(&g.local_waiting, __ATOMIC_RELAXED); tick++) usleep(10000);
    local_http_stop();
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

static void run_network(void)
{
    static const struct vnd_settings joining = {8, 1, 16 * 1024};
    /* One transfer at a time, eight receive transfers: on the console 3.2 Gbit/s up and
     * 3.0 down, where thirty-two gave 2.6 up and 3.2 down, and uploads are what this is
     * for. Two transfers at a time were slower, and were in use when an early build
     * powered the console off. */
    static const struct vnd_settings serving = {8, 1, VND_TX_BYTES};
    /* Linux's aggregate size: nothing larger has been sent to an RTL8156B or RTL8157. */
    static const struct vnd_settings serving_linux = {8, 1, VND_LINUX_TX_BYTES};
    char text[96];

    vnd_configure(&joining);
    if (wait_for_link() != 0) {
        say("no Ethernet link");
        notify("网线未连接");
        return;
    }
    g.link_up = 1;
    g.link_mbps = rtl_phy_mbps(vnd_regs_phy_status());
    vnd_regs_mark_link(1);
    net_link(1);
    usleep(LINK_SETTLE_US);
    say("link up, %u Mbit/s", g.link_mbps);
    if (vnd_regs_open_filter() != 0) {
        say("the packet filter could not be set");
        return;
    }
    if (net_acquire(ADDRESS_WAIT_MS, &g.lease) != 0) {
        say("no address");
        notify("未获取到 IP 地址");
        return;
    }
    say("address from %s", g.lease.source);
    console_line("LAN address: %s, http://%s/", g.lease.ip, g.lease.ip);
    checkpoint();
    snprintf(text, sizeof text, "http://%s/", g.lease.ip);
    notify(text);
    files_set_log(log_file_line);
    smb_set_store(SMB_STORE);
    say("NAS password key %s", secret_ready() ? "ready" : "unavailable");
    smb_client_use_adapter(1);
    vnd_configure(g.chip == RTL_8159 ? &serving : &serving_linux);
    crash_stage("taking rights");
    if (tile_elevate() == 0) {
        files_sweep();
        crash_stage("starting the local page");
        start_local_page();
        tile_resume();
    }
    forward_set_store(FORWARD_STORE);
    if (forward_start(g.lease.ip) != 0) say("port forwarding did not start");
    crash_stage("serving");
    if (net_serve(&g_backend, keep_going) != 0) say("the web server did not start: errno %d", errno);
    forward_stop();
    files_stop();
    stop_local_page();
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

static void announce(void)
{
    char text[96];

    native_address(g.native_ip, sizeof g.native_ip);
    if (g.native_ip[0]) {
        console_line("console address: %s, http://%s:%d/", g.native_ip, g.native_ip, TILE_PORT);
        snprintf(text, sizeof text, "http://%s:%d/", g.native_ip, TILE_PORT);
    } else {
        say("the console's address is not known");
        snprintf(text, sizeof text, "端口 %d：用主机的 IP 地址打开", TILE_PORT);
    }
    notify(text);
    checkpoint();
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
    notify(result || g.native ? "已退出" : "已退出，网卡已恢复");
    log_close();
    return result;
}
