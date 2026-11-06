/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tom-ps5-usbnet: a file manager for a PS5, served over a USB 10 GbE adapter that has
 * its own address on the LAN. Runs only when the console's owner sends it to their ELF
 * loader.
 *
 * The page has no login: whoever reaches the port on the LAN can read and delete files
 * on the console. See files.c. The run ends from the page, when the adapter goes away,
 * or when the console sleeps.
 *
 * MAC addresses have to be in the frames and are never logged. */
#include "adapter.h"
#include "crash.h"
#include "files.h"
#include "local_http.h"
#include "log.h"
#include "net.h"
#include "tile.h"
#include "vendor.h"

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
#define PHY_10G 10000u
#define CONFIG_BYTES 2048

static struct {
    struct net_lease lease;
    uint8_t mac[6];
    unsigned link_mbps;
    int stop, link_up, local_waiting, retry_stop;
    unsigned bulk_in, bulk_out;
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

static int info(void *context, char *out, size_t capacity)
{
    (void)context;
    snprintf(out, capacity, "{\"name\":\"" APP_NAME "\",\"version\":\"" APP_VERSION "\",\"ip\":\"%s\",\"link_mbps\":%u}", g.lease.ip, g.link_mbps);
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
    return files_route(request, stream);
}

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
        if (!__atomic_load_n(&g.retry_stop, __ATOMIC_RELAXED) && local_http_start(&g_backend, TILE_PORT) == 0) {
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

    if (local_http_start(&g_backend, TILE_PORT) == 0) return;
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

static int wait_for_link(void)
{
    long started = now_ms();
    int up = 0;

    while (now_ms() - started < LINK_WAIT_MS && !vnd_gone()) {
        uint16_t status = vnd_regs_phy_status();

        if (rtl_phy_link(status)) {
            up = 1;
            if (rtl_phy_mbps(status) == PHY_10G) break;
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
    vnd_configure(&serving);
    crash_stage("taking rights");
    if (tile_elevate() == 0) {
        files_sweep();
        crash_stage("starting the local page");
        start_local_page();
        tile_resume();
    }
    crash_stage("serving");
    if (net_serve(&g_backend, keep_going) != 0) say("the web server did not start: errno %d", errno);
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

static void run(struct adapter *a)
{
    static const struct vnd_hooks hooks = {net_frames};
    static const struct link_ops link = {vnd_tx_reserve, vnd_tx_commit};
    int vendor = find_vendor(a);

    if (vendor < 0) {
        say("this adapter has no vendor configuration");
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
    vnd_regs_attach(a->fd);
    if (vnd_regs_bring_up(g.mac) != 0) {
        say("chip bring-up failed");
        notify("网卡初始化失败");
        return;
    }
    if (vnd_open(a->fd, g.bulk_in, g.bulk_out, &hooks) == 0) {
        if (net_start(g.mac, &link) != 0) say("the network stack did not start: errno %d", errno);
        else if (vnd_start() == 0) run_network();
    }
    vnd_regs_stop();
    vnd_close();
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
    say("stopped");
    checkpoint();
    notify(result ? "已退出" : "已退出，网卡已恢复");
    log_close();
    return result;
}
