/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Runs the whole program on the host: real receive thread, lwIP and HTTP, against a
 * simulated ugen node. The simulated chip holds registers,
 * passes frames only once it has been brought up in the right order, refuses writes
 * off the list below, and frames its bulk transfers as the driver expects. All of
 * that comes from reading Linux's driver. It proves nothing about the real chip. */
#include "adapter.h"
#include "vendor.h"
#include "log.h"
#include "mock_descriptors.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>

int app_main(void);
int peer_main(int fd);

#undef ioctl
#undef open
#undef close
#undef stat
#undef poll
#undef unlink

#define MAX_EP 128
#define MOCK_FD 1000
#define FIFO_WAIT_NS 2000000L
#define PLA 1
#define USB 0
#define MAX_AGGREGATE 40
#define FRAME_BYTES 2048

static struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int cable, stop;
    int config, target, alt, fs_active;
    struct usb_fs_endpoint *ep;
    int ep_max, opened[MAX_EP];
    unsigned ep_no[MAX_EP], max_frames[MAX_EP], bufsize[MAX_EP];
    int rx_queue[MAX_EP], rx_count, done[MAX_EP * 2], done_count;
    unsigned long xmit_ok, rcv_ok;
    unsigned long rx_dropped, aggregates, largest, multi_frame_transfers, writes;
    uint32_t regs[2][0x4000], bmu;
    int configs_seen[4], fs_uninit;
    long link_at;
    pthread_t receiver;
} m;

static char report[1 << 20], console[4096];
static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;
static char last_notify[160];

static long now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int fail(int code)
{
    errno = code;
    return -1;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void finish(int i, uint32_t len, int status)
{
    m.ep[i].pLength[0] = len;
    m.ep[i].aFrames = 1;
    m.ep[i].status = (uint8_t)status;
    assert(m.done_count < MAX_EP * 2);
    m.done[m.done_count++] = i;
    pthread_cond_broadcast(&m.changed);
}

static uint32_t *reg(unsigned which, unsigned address)
{
    assert(address >= 0xb000 && !(address & 3));
    return &m.regs[which][(address - 0xb000) / 4];
}

static int chip_running(void)
{
    return m.config == 0 && (*reg(PLA, 0xe810) >> 24 & 0x0c) == 0x0c && !(*reg(PLA, 0xe858) >> 16 & 8) && (*reg(PLA, 0xc010) & 0x0f);
}

/* The chip gathers what has arrived, up to the early size, and hands it to the
 * oldest pending transfer. */
static void *receiver(void *unused)
{
    static uint8_t frames[MAX_AGGREGATE][FRAME_BYTES];
    struct rtl_frame list[MAX_AGGREGATE];

    (void)unused;
    for (;;) {
        ssize_t n = recv(m.cable, frames[0], FRAME_BYTES, 0);
        size_t count = 1, written = 0, bytes;
        size_t early;

        if (n <= 0) return NULL;
        list[0].data = frames[0];
        list[0].len = (size_t)n;
        bytes = (size_t)n;
        pthread_mutex_lock(&m.lock);
        m.rcv_ok++;
        early = (*reg(USB, 0xd42c) >> 16) * 16u;
        while (count < MAX_AGGREGATE && bytes + 2 * FRAME_BYTES < early) {
            n = recv(m.cable, frames[count], FRAME_BYTES, MSG_DONTWAIT);
            if (n <= 0) break;
            m.rcv_ok++;
            list[count].data = frames[count];
            list[count++].len = (size_t)n;
            bytes += (size_t)n;
        }
        if (!chip_running()) {
            m.rx_dropped += count;
            pthread_mutex_unlock(&m.lock);
            continue;
        }
        /* The adapter holds a frame for a moment while no transfer is pending. */
        if (!m.rx_count) {
            struct timespec until;

            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += FIFO_WAIT_NS;
            until.tv_sec += until.tv_nsec / 1000000000L;
            until.tv_nsec %= 1000000000L;
            while (!m.rx_count && !m.stop)
                if (pthread_cond_timedwait(&m.changed, &m.lock, &until)) break;
        }
        if (m.rx_count) {
            int i = m.rx_queue[0];

            memmove(m.rx_queue, m.rx_queue + 1, sizeof m.rx_queue[0] * (size_t)--m.rx_count);
            assert(rtl_rx_fixture_encode(RTL_8159, list, count, m.ep[i].ppBuffer[0], m.ep[i].pLength[0], &written) == 0);
            m.aggregates++;
            if (count > m.largest) m.largest = count;
            finish(i, (uint32_t)written, 0);
        } else {
            m.rx_dropped += count;
        }
        pthread_mutex_unlock(&m.lock);
    }
}

static void send_aggregate(int i)
{
    static struct rtl_frame_view views[800];
    struct usb_fs_endpoint *ep = &m.ep[i];
    size_t count = 0, k;
    uint32_t length = ep->pLength[0];

    assert(chip_running() || (*reg(PLA, 0xe810) >> 24 & 0x04));
    assert(ep->nFrames == 1 && ep->flags == 0 && ep->timeout > 0 && length <= m.bufsize[i]);
    assert(rtl_tx_decode(RTL_8159, ep->ppBuffer[0], length, views, 800, &count) == 0 && count >= 1);
    if (count > 1) m.multi_frame_transfers++;
    for (k = 0; k < count; k++) {
        assert(views[k].len >= 60 && views[k].len <= 1514);
        for (;;) {
            if (send(m.cable, views[k].data, views[k].len, 0) == (ssize_t)views[k].len) break;
            /* The peer leaves after its last check, and the run takes a moment to end. */
            if (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN || errno == ECONNREFUSED) break;
            assert(errno == ENOBUFS || errno == EAGAIN || errno == EINTR);
            usleep(100);
        }
        m.xmit_ok++;
    }
    ep->aFrames = 1;
    ep->status = 0;
    assert(m.done_count < MAX_EP * 2);
    m.done[m.done_count++] = i;
    pthread_cond_broadcast(&m.changed);
}

int mock_stat(const char *path, struct stat *st)
{
    (void)st;
    return strcmp(path, "/dev/ugen2.2") ? fail(ENOENT) : 0;
}

int mock_open(const char *path, int flags, ...)
{
    (void)flags;
    return strcmp(path, "/dev/ugen2.2") ? fail(ENXIO) : MOCK_FD;
}

int mock_close(int fd)
{
    return fd == MOCK_FD ? 0 : close(fd);
}

int mock_unlink(const char *path)
{
    (void)path;
    return fail(ENOENT);
}

int mock_poll(struct pollfd *fds, nfds_t n, int timeout)
{
    struct timespec until;
    int ready;

    assert(n == 1 && fds[0].fd == MOCK_FD && timeout > 0);
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += timeout * 1000000L;
    until.tv_sec += until.tv_nsec / 1000000000L;
    until.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&m.lock);
    while (!m.done_count)
        if (pthread_cond_timedwait(&m.changed, &m.lock, &until)) break;
    ready = m.done_count > 0;
    pthread_mutex_unlock(&m.lock);
    fds[0].revents = ready ? POLLIN : 0;
    return ready;
}

static int vendor_read(struct usb_ctl_request *c)
{
    uint8_t *p = c->ucr_data;
    unsigned address = UGETW(c->ucr_request.wValue), length = UGETW(c->ucr_request.wLength);
    unsigned which = UGETW(c->ucr_request.wIndex) >> 8 ? PLA : USB, i;

    assert(c->ucr_request.bRequest == 0x05);
    memset(p, 0, length);
    if (which == PLA && address == 0xe908) {
        put_le32(p, now() >= m.link_at ? 0x00094083u : 0);
    } else {
        for (i = 0; i < length / 4; i++) put_le32(p + 4 * i, *reg(which, address + 4 * i));
    }
    c->ucr_actlen = (uint16_t)length;
    return 0;
}

/* Written independently of the tables in vendor_regs.c, from the same driver source. */
static int on_the_list(unsigned which, unsigned dword)
{
    static const uint16_t pla[] = {0xc000, 0xc004, 0xc010, 0xc014, 0xc0a0, 0xc0a4, 0xc0a8, 0xc0b4, 0xc0bc, 0xcd00, 0xcd04, 0xd2cc, 0xd2e8, 0xd398,
        0xe080, 0xe0cc, 0xe428, 0xe610, 0xe614, 0xe618, 0xe800, 0xe810, 0xe81c, 0xe84c, 0xe858, 0xe8dc};
    static const uint16_t usb[] = {0xcbfc, 0xcfd8, 0xcfec, 0xcffc, 0xd268, 0xd3c8, 0xd404, 0xd40c, 0xd42c, 0xd430, 0xd434, 0xd4ac, 0xd4b0, 0xd4d8, 0xd4ec,
        0xd5d4, 0xd5d8, 0xd5dc, 0xd808, 0xd818};
    const uint16_t *list = which == PLA ? pla : usb;
    size_t i, n = which == PLA ? sizeof pla / sizeof pla[0] : sizeof usb / sizeof usb[0];

    for (i = 0; i < n; i++)
        if (list[i] == dword) return 1;
    return 0;
}

static int vendor_write(struct usb_ctl_request *c)
{
    const uint8_t *p = c->ucr_data;
    unsigned dword = UGETW(c->ucr_request.wValue), index = UGETW(c->ucr_request.wIndex);
    unsigned which = index >> 8 ? PLA : USB, enable = index & 0x0f, i;
    uint32_t value = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24, mask = 0;
    uint32_t *r;

    assert(c->ucr_request.bRequest == 0x05 && UGETW(c->ucr_request.wLength) == 4 && !(dword & 3) && (index & 0xff) >> 4 == enable);
    assert(m.config == 0);
    if (!on_the_list(which, dword)) {
        fprintf(stderr, "write off the list: %u %#x\n", which, dword);
        abort();
    }
    m.writes++;
    for (i = 0; i < 4; i++)
        if (enable & (1u << i)) mask |= 0xffu << (i * 8);
    r = reg(which, dword);
    *r = (*r & ~mask) | (value & mask);
    if (which == USB && dword == 0xd5dc && (mask & 0xffff)) {   /* the BMU's command register */
        if (*r & 2) m.bmu = *reg(USB, 0xd5d8);
        else *reg(USB, 0xd5d8) = m.bmu;
        *r &= ~3u;
    }
    if (which == PLA && dword == 0xc010 && (*r & 0x0f)) assert(m.rx_count > 0);
    c->ucr_actlen = 4;
    return 0;
}

static int control(struct usb_ctl_request *c)
{
    uint8_t type = c->ucr_request.bmRequestType, request = c->ucr_request.bRequest;

    if (type == 0xc0) return vendor_read(c);
    if (type == 0x40) return vendor_write(c);
    if (type == 0xa1 && request == NCM_GET_ETHERNET_STATISTIC) {
        unsigned selector = UGETW(c->ucr_request.wValue);

        put_le32(c->ucr_data, selector == NCM_STAT_XMIT_OK ? (uint32_t)m.xmit_ok : selector == NCM_STAT_RCV_OK ? (uint32_t)m.rcv_ok : 0);
        c->ucr_actlen = 4;
        return 0;
    }
    /* The one class request, while passing through ECM. Anything else is a bug. */
    assert(type == 0x21 && request == NCM_SET_PACKET_FILTER && m.alt == 1 && UGETW(c->ucr_request.wLength) == 0);
    return 0;
}

static int full_desc(struct usb_gen_descriptor *d)
{
    const uint8_t *data = d->ugd_config_index == 2 ? captured_ecm : d->ugd_config_index == 1 ? captured_ssp : captured_vendor_hs;
    size_t len = d->ugd_config_index == 2 ? sizeof captured_ecm : d->ugd_config_index == 1 ? sizeof captured_ssp : sizeof captured_vendor_hs;

    if (len > d->ugd_maxlen) len = d->ugd_maxlen;
    memcpy(d->ugd_data, data, len);
    d->ugd_actlen = (uint16_t)len;
    return 0;
}

static int string_desc(struct usb_gen_descriptor *d)
{
    static const char digits[] = "02AABBCCDDEE";
    uint8_t *p = d->ugd_data;
    int i;

    assert(d->ugd_string_index == 3);
    p[0] = 26;
    p[1] = 3;
    for (i = 0; i < 12; i++) {
        p[2 + 2 * i] = (uint8_t)digits[i];
        p[3 + 2 * i] = 0;
    }
    d->ugd_actlen = 26;
    return 0;
}

static int fs_open(struct usb_fs_open *o)
{
    assert(m.fs_active && o->ep_index < m.ep_max && !m.opened[o->ep_index]);
    assert((o->ep_no == 0x81 || o->ep_no == 0x02) && o->max_frames == 1);
    o->max_packet_length = 1024;
    m.opened[o->ep_index] = 1;
    m.ep_no[o->ep_index] = o->ep_no;
    m.max_frames[o->ep_index] = o->max_frames;
    m.bufsize[o->ep_index] = o->max_bufsize;
    return 0;
}

static int fs_start(const struct usb_fs_start *start)
{
    int i = start->ep_index;

    assert(m.fs_active && m.opened[i]);
    if (m.ep_no[i] == 0x02) {
        send_aggregate(i);
    } else {
        assert(m.ep[i].nFrames == 1 && m.ep[i].flags == USB_FS_FLAG_SINGLE_SHORT_OK && m.ep[i].pLength[0] == VND_RX_BYTES);
        m.rx_queue[m.rx_count++] = i;
        pthread_cond_broadcast(&m.changed);
    }
    return 0;
}

static int fs_complete(struct usb_fs_complete *done)
{
    if (!m.done_count) return fail(EBUSY);
    done->ep_index = (uint8_t)m.done[0];
    memmove(m.done, m.done + 1, sizeof m.done[0] * (size_t)--m.done_count);
    return 0;
}

static int device_ioctl(unsigned long request, void *arg)
{
    switch (request) {
    case USB_GET_DEVICE_DESC: {
        struct usb_device_descriptor *dd = arg;

        memset(dd, 0, sizeof *dd);
        USETW(dd->idVendor, 0x0bda);
        USETW(dd->idProduct, 0x815a);
        USETW(dd->bcdUSB, 0x0320);
        dd->bNumConfigurations = 3;
        return 0;
    }
    case USB_GET_FULL_DESC:
        return full_desc(arg);
    case USB_GET_STRING_DESC:
        return string_desc(arg);
    case USB_GET_CONFIG:
        *(int *)arg = m.config;
        return 0;
    case USB_SET_CONFIG:
        m.config = *(int *)arg;
        m.configs_seen[m.config]++;
        m.alt = 0;
        m.link_at = now() + 300;
        *reg(PLA, 0xe810) = m.config == 2 ? 0x0c000000u : 0;
        return 0;
    case USB_IFACE_DRIVER_ACTIVE:
        return fail(ENXIO);
    case USB_SET_ALTINTERFACE:
        m.alt = ((struct usb_alt_interface *)arg)->uai_alt_index;
        return 0;
    case USB_DO_REQUEST:
        return control(arg);
    case USB_FS_INIT: {
        struct usb_fs_init *init = arg;

        assert(!m.fs_active && m.config == 0 && init->ep_index_max <= 127);
        m.ep = init->pEndpoints;
        m.ep_max = init->ep_index_max;
        m.fs_active = 1;
        return 0;
    }
    case USB_FS_OPEN:
        return fs_open(arg);
    case USB_FS_START:
        return fs_start(arg);
    case USB_FS_COMPLETE:
        return fs_complete(arg);
    case USB_FS_UNINIT:
        m.fs_active = 0;
        m.fs_uninit++;
        m.rx_count = m.done_count = 0;
        return 0;
    default:
        assert(!"an ioctl off the list");
        return fail(ENOTTY);
    }
}

int mock_ioctl(int fd, unsigned long request, void *arg)
{
    int result;

    assert(fd == MOCK_FD);
    pthread_mutex_lock(&m.lock);
    result = device_ioctl(request, arg);
    pthread_mutex_unlock(&m.lock);
    return result;
}

/* ---- log stubs ----------------------------------------------------------- */

int log_open(void) { return 0; }
void log_close(void) {}
void log_firmware(void) {}
void checkpoint(void) {}
void notify(const char *text) { snprintf(last_notify, sizeof last_notify, "%s", text); }

void console_line(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(console, sizeof console, fmt, ap);
    va_end(ap);
}

void say(const char *fmt, ...)
{
    va_list ap;
    size_t used;

    pthread_mutex_lock(&report_lock);
    used = strlen(report);
    va_start(ap, fmt);
    used += (size_t)vsnprintf(report + used, sizeof report - used - 2, fmt, ap);
    va_end(ap);
    report[used] = '\n';
    report[used + 1] = 0;
    pthread_mutex_unlock(&report_lock);
}

extern const char *peer_files_root, *files_test_root;
extern int tile_calls[4];

int main(void)
{
    int cable[2], size = 1 << 20, status, code;
    char files_root[64];
    pid_t child;

    alarm(120);
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, cable) == 0);
    setsockopt(cable[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof size);
    setsockopt(cable[1], SOL_SOCKET, SO_SNDBUF, &size, sizeof size);
    setsockopt(cable[0], SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
    setsockopt(cable[1], SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
    snprintf(files_root, sizeof files_root, "/tmp/tom-usbnet-files-%d", (int)getpid());
    assert(mkdir(files_root, 0700) == 0);
    peer_files_root = files_test_root = files_root;
    child = fork();                     /* before any thread in this process */
    assert(child >= 0);
    if (child == 0) {
        close(cable[0]);
        _exit(peer_main(cable[1]));
    }
    close(cable[1]);
    pthread_mutex_init(&m.lock, NULL);
    pthread_cond_init(&m.changed, NULL);
    m.cable = cable[0];
    m.config = 0;
    /* As read from the chip in the vendor configuration, and the adapter's own address. */
    *reg(PLA, 0xc000) = 0xccbbaa02u;
    *reg(PLA, 0xc004) = 0x0000eeddu;
    *reg(PLA, 0xc010) = 0x0402480fu;
    *reg(PLA, 0xe004) = 0x00000002u;
    *reg(PLA, 0xe610) = 0x23200800u;
    *reg(PLA, 0xe858) = 0x02000000u;
    *reg(USB, 0xd404) = 0x00402e00u;
    *reg(USB, 0xd40c) = 0x00600400u;
    *reg(USB, 0xd4b0) = 0x000300ffu;
    pthread_create(&m.receiver, NULL, receiver, NULL);
    code = app_main();
    pthread_mutex_lock(&m.lock);
    m.stop = 1;
    pthread_mutex_unlock(&m.lock);
    assert(waitpid(child, &status, 0) == child);
    fputs(report, stderr);
    assert(code == 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(strstr(report, "link up, 10000 Mbit/s"));
    assert(strstr(report, "address from dhcp"));
    assert(strstr(console, "http://192.0.2.50/"));
    assert(!strstr(report, "192.0.2.50"));              /* the address stays out of the file */
    assert(strstr(report, ": 17825799 bytes in"));
    assert(strstr(report, "upload /tmp"));
    assert(strstr(report, "download /tmp"));
    assert(strstr(report, "disk /tmp"));
    assert(!strstr(report, "write failed") && !strstr(report, "up.bin"));
    assert(!strcasestr(report, "aa:bb:cc") && !strcasestr(report, "aabbcc"));
    assert(strstr(last_notify, "已恢复"));
    fprintf(stderr, "mock: %lu register writes, largest aggregate %lu frames, %lu frames dropped\n", m.writes, m.largest, m.rx_dropped);
    assert(m.multi_frame_transfers > 0 && m.largest > 1 && m.writes > 20);
    assert(m.fs_uninit == 1 && m.configs_seen[2] == 1 && m.configs_seen[0] == 1 && m.config == 0 && m.alt == 0);
    assert(tile_calls[0] == 1 && tile_calls[1] == 1 && tile_calls[3] == 1);
    assert(rmdir(files_root) == 0);     /* the peer deleted what it uploaded */
    puts("end-to-end test passed");
    return 0;
}
