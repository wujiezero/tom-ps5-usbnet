/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The far end of test_app.c's simulated cable: a second lwIP with a small DHCP
 * server and a client that does what the page does. */
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "netif/ethernet.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

ssize_t recv(int, void *, size_t, int);
ssize_t send(int, const void *, size_t, int);

extern unsigned char *net_heap;

#define SERVER_IP PP_HTONL(LWIP_MAKEU32(192, 0, 2, 1))
#define CLIENT_IP PP_HTONL(LWIP_MAKEU32(192, 0, 2, 50))
#define DHCP_MAGIC 0x63825363u
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "peer: %s line %d\n", #condition, __LINE__); _exit(2); } } while (0)

const char *peer_files_root;
unsigned peer_link_mbps = 10000;      /* what the simulated chip reports */

static struct netif g_netif;
static int g_cable;

static err_t link_output(struct netif *netif, struct pbuf *packet)
{
    unsigned char frame[1600];

    (void)netif;
    pbuf_copy_partial(packet, frame, packet->tot_len, 0);
    for (;;) {
        if (send(g_cable, frame, packet->tot_len, 0) == packet->tot_len) return ERR_OK;
        if (errno != ENOBUFS && errno != EAGAIN && errno != EINTR) return ERR_IF;
        sys_msleep(1);
    }
}

static err_t interface_init(struct netif *netif)
{
    static const unsigned char mac[6] = {2, 0x11, 0x22, 0x33, 0x44, 0x55};

    netif->name[0] = 'p';
    netif->name[1] = 'r';
    netif->output = etharp_output;
    netif->linkoutput = link_output;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, mac, 6);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

static void *cable_reader(void *unused)
{
    static unsigned char frame[2048];

    (void)unused;
    for (;;) {
        ssize_t size = recv(g_cable, frame, sizeof frame, 0);
        struct pbuf *packet;

        if (size <= 0) return NULL;
        packet = pbuf_alloc(PBUF_RAW, (u16_t)size, PBUF_POOL);
        if (!packet) continue;
        pbuf_take(packet, frame, (u16_t)size);
        if (g_netif.input(packet, &g_netif) != ERR_OK) pbuf_free(packet);
    }
}

static void core_ready(void *semaphore)
{
    sys_sem_signal(semaphore);
}

static void stack_start(void)
{
    sys_sem_t ready;
    ip4_addr_t address, mask, gateway;
    pthread_t thread;

    CHECK(sys_sem_new(&ready, 0) == ERR_OK);
    tcpip_init(core_ready, &ready);
    sys_sem_wait(&ready);
    sys_sem_free(&ready);
    IP4_ADDR(&address, 192, 0, 2, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gateway, 0, 0, 0, 0);
    LOCK_TCPIP_CORE();
    CHECK(netif_add(&g_netif, &address, &mask, &gateway, NULL, interface_init, tcpip_input));
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);
    UNLOCK_TCPIP_CORE();
    pthread_create(&thread, NULL, cable_reader, NULL);
}

/* ---- DHCP ---------------------------------------------------------------- */

static unsigned put_option(unsigned char *p, unsigned at, unsigned code, const void *data, unsigned len)
{
    p[at] = (unsigned char)code;
    p[at + 1] = (unsigned char)len;
    memcpy(p + at + 2, data, len);
    return at + 2 + len;
}

static void *dhcp_server(void *unused)
{
    struct sockaddr_in address;
    int fd = lwip_socket(AF_INET, SOCK_DGRAM, 0), on = 1;

    (void)unused;
    memset(&address, 0, sizeof address);
    address.sin_len = sizeof address;
    address.sin_family = AF_INET;
    address.sin_port = lwip_htons(67);
    CHECK(fd >= 0 && lwip_setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on) == 0);
    CHECK(lwip_bind(fd, (struct sockaddr *)&address, sizeof address) == 0);
    for (;;) {
        unsigned char in[576], out[576];
        ssize_t size = lwip_recv(fd, in, sizeof in, 0);
        unsigned at = 240, type = 0;
        const uint32_t server = SERVER_IP, client = CLIENT_IP, mask = PP_HTONL(LWIP_MAKEU32(255, 255, 255, 0)), lease = PP_HTONL(3600);
        unsigned char reply;

        if (size < 244) continue;
        while (at + 2 <= (unsigned)size && in[at] != 255) {
            if (in[at] == 53) type = in[at + 2];
            at += 2u + in[at + 1];
        }
        if (type != 1 && type != 3) continue;
        reply = type == 1 ? 2 : 5;
        memset(out, 0, sizeof out);
        out[0] = 2;
        out[1] = 1;
        out[2] = 6;
        memcpy(out + 4, in + 4, 4);
        memcpy(out + 16, &client, 4);
        memcpy(out + 28, in + 28, 16);
        out[236] = 0x63;
        out[237] = 0x82;
        out[238] = 0x53;
        out[239] = 0x63;
        at = put_option(out, 240, 53, &reply, 1);
        at = put_option(out, at, 54, &server, 4);
        at = put_option(out, at, 51, &lease, 4);
        at = put_option(out, at, 1, &mask, 4);
        at = put_option(out, at, 3, &server, 4);
        out[at++] = 255;
        address.sin_addr.s_addr = PP_HTONL(IPADDR_BROADCAST);
        address.sin_port = lwip_htons(68);
        lwip_sendto(fd, out, at < 300 ? 300 : at, 0, (struct sockaddr *)&address, sizeof address);
    }
}

/* ---- the client ---------------------------------------------------------- */

struct reply {
    char head[1024], body[2048];
    int status;
    size_t total;
};

static int connect_app(void)
{
    struct sockaddr_in address;
    struct timeval timeout = {30, 0};
    int tries;

    memset(&address, 0, sizeof address);
    address.sin_len = sizeof address;
    address.sin_family = AF_INET;
    address.sin_port = lwip_htons(80);
    address.sin_addr.s_addr = CLIENT_IP;
    for (tries = 0; tries < 600; tries++) {
        int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);

        CHECK(fd >= 0);
        lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        if (lwip_connect(fd, (struct sockaddr *)&address, sizeof address) == 0) return fd;
        lwip_close(fd);
        sys_msleep(100);
    }
    CHECK(!"connected");
    return -1;
}

static void write_all(int fd, const void *data, size_t length)
{
    const char *p = data;

    while (length) {
        ssize_t n = lwip_write(fd, p, length);
        CHECK(n > 0);
        p += n;
        length -= (size_t)n;
    }
}

static void read_reply(int fd, struct reply *r)
{
    static char data[1 << 16];
    size_t used = 0, kept = 0, head_end = 0;
    ssize_t n;

    memset(r, 0, sizeof *r);
    while ((n = lwip_read(fd, data + (head_end ? 0 : used), head_end ? sizeof data : sizeof data - used)) > 0) {
        if (!head_end) {
            char *end;

            used += (size_t)n;
            for (end = data; end + 3 < data + used && memcmp(end, "\r\n\r\n", 4); end++) {}
            if (end + 3 >= data + used) continue;
            head_end = (size_t)(end + 4 - data);
            memcpy(r->head, data, head_end < sizeof r->head ? head_end : sizeof r->head - 1);
            n = (ssize_t)(used - head_end);
            memmove(data, data + head_end, (size_t)n);
        }
        if (kept + 1 < sizeof r->body) {
            size_t take = (size_t)n < sizeof r->body - 1 - kept ? (size_t)n : sizeof r->body - 1 - kept;
            memcpy(r->body + kept, data, take);
            kept += take;
        }
        r->total += (size_t)n;
    }
    CHECK(head_end);
    CHECK(sscanf(r->head, "HTTP/1.1 %d", &r->status) == 1);
}

static void request(const char *method, const char *target, const char *extra, struct reply *r)
{
    char head[512];
    int fd = connect_app();
    int n = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: 192.0.2.50\r\n%sConnection: close\r\n\r\n", method, target, extra);

    write_all(fd, head, (size_t)n);
    read_reply(fd, r);
    lwip_close(fd);
}

static void upload(const char *target, size_t size, struct reply *r)
{
    static char chunk[65536];
    char head[256];
    int fd = connect_app();
    int n = snprintf(head, sizeof head, "POST %s HTTP/1.1\r\nHost: 192.0.2.50\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", target, size);
    size_t left = size;

    memset(chunk, 0x33, sizeof chunk);
    write_all(fd, head, (size_t)n);
    while (left) {
        size_t now = left < sizeof chunk ? left : sizeof chunk;
        write_all(fd, chunk, now);
        left -= now;
    }
    read_reply(fd, r);
    lwip_close(fd);
}

static long json_number(const char *json, const char *key)
{
    char wanted[32];
    const char *p;

    snprintf(wanted, sizeof wanted, "\"%s\":", key);
    p = strstr(json, wanted);
    CHECK(p);
    return strtol(p + strlen(wanted), NULL, 10);
}

/* The file routes over the same path as everything else. */
static void files(void)
{
    enum { SIZE = 17 * 1024 * 1024 + 7 };   /* past the size that gets an fsync */
    char target[256];
    struct reply r;

    request("GET", "/fs/volumes", "", &r);
    CHECK(r.status == 200 && strstr(r.body, peer_files_root));
    snprintf(target, sizeof target, "/fs/mkdir?path=%s/sub", peer_files_root);
    request("POST", target, "", &r);
    CHECK(r.status == 200);
    snprintf(target, sizeof target, "/fs/put?path=%s/sub/up.bin&client=ab12", peer_files_root);
    upload(target, SIZE, &r);
    CHECK(r.status == 200 && json_number(r.body, "bytes") == SIZE);
    snprintf(target, sizeof target, "/fs/list?path=%s/sub", peer_files_root);
    request("GET", target, "", &r);
    CHECK(r.status == 200 && strstr(r.body, "\"name\":\"up.bin\"") && json_number(r.body, "size") == SIZE);
    snprintf(target, sizeof target, "/fs/get?path=%s/sub/up.bin", peer_files_root);
    request("GET", target, "", &r);
    CHECK(r.status == 200 && r.total == SIZE && r.body[0] == 0x33);
    request("GET", target, "Range: bytes=1048576-1048585\r\n", &r);
    CHECK(r.status == 206 && r.total == 10);
    request("GET", "/fs/transfers", "", &r);
    CHECK(r.status == 200 && strstr(r.body, "\"kind\":\"upload\",\"client\":\"ab12\",\"name\":\"up.bin\""));
    CHECK(strstr(r.body, "\"kind\":\"download\"") && json_number(r.body, "done") == SIZE);
    snprintf(target, sizeof target, "/fs/bench?path=%s", peer_files_root);
    request("POST", target, "", &r);
    CHECK(r.status == 200 && strstr(r.body, "\"write\":"));
    snprintf(target, sizeof target, "/fs/delete?path=%s/sub", peer_files_root);
    request("POST", target, "", &r);
    CHECK(r.status == 200);
    request("GET", "/tile/status", "", &r);
    CHECK(r.status == 200 && strstr(r.body, "installed"));
    request("POST", "/tile/remove", "", &r);
    CHECK(r.status == 200);
}

/* ---- a NAS that turns the console away -------------------------------------- */
/* It answers the first SMB2 request, the NEGOTIATE, with STATUS_ACCESS_DENIED: enough
 * for libsmb2 to go through every socket call the console's side makes on lwIP. */
static int g_nas_negotiated;

static int read_exactly(int fd, unsigned char *p, size_t length)
{
    while (length) {
        ssize_t n = lwip_read(fd, p, length);

        if (n <= 0) return -1;
        p += n;
        length -= (size_t)n;
    }
    return 0;
}

static void *fake_nas(void *unused)
{
    struct sockaddr_in address;
    int listener = lwip_socket(AF_INET, SOCK_STREAM, 0), one = 1;

    (void)unused;
    CHECK(listener >= 0);
    lwip_setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&address, 0, sizeof address);
    address.sin_len = sizeof address;
    address.sin_family = AF_INET;
    address.sin_port = lwip_htons(445);
    address.sin_addr.s_addr = SERVER_IP;
    CHECK(lwip_bind(listener, (struct sockaddr *)&address, sizeof address) == 0);
    CHECK(lwip_listen(listener, 1) == 0);
    for (;;) {
        static unsigned char request[65536];
        unsigned char reply[4 + 64 + 9];
        struct sockaddr_in from;
        socklen_t from_length = sizeof from;
        int fd = lwip_accept(listener, (struct sockaddr *)&from, &from_length);
        size_t length;

        CHECK(fd >= 0);
        CHECK(from.sin_addr.s_addr == CLIENT_IP);
        if (read_exactly(fd, request, 4) == 0) {
            length = (size_t)request[1] << 16 | (size_t)request[2] << 8 | request[3];
            CHECK(request[0] == 0 && length >= 64 && length <= sizeof request);
            CHECK(read_exactly(fd, request, length) == 0);
            CHECK(!memcmp(request, "\xfeSMB", 4) && request[12] == 0 && request[13] == 0);
            memset(reply, 0, sizeof reply);
            reply[3] = 64 + 9;
            memcpy(reply + 4, "\xfeSMB", 4);
            reply[4 + 4] = 64;                          /* StructureSize */
            reply[4 + 8] = 0x22;                        /* STATUS_ACCESS_DENIED, LE */
            reply[4 + 11] = 0xc0;
            reply[4 + 14] = 1;                          /* one credit */
            reply[4 + 16] = 1;                          /* from the server */
            memcpy(reply + 4 + 24, request + 24, 8);    /* MessageId */
            reply[4 + 64] = 9;                          /* the error body */
            write_all(fd, reply, sizeof reply);
            __atomic_add_fetch(&g_nas_negotiated, 1, __ATOMIC_RELAXED);
            while (lwip_read(fd, request, sizeof request) > 0) {}
        }
        lwip_close(fd);
    }
    return NULL;
}

static void post_form(const char *target, const char *form, struct reply *r)
{
    char head[512];
    int fd = connect_app();
    int n = snprintf(head, sizeof head, "POST %s HTTP/1.1\r\nHost: 192.0.2.50\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", target, strlen(form));

    write_all(fd, head, (size_t)n);
    write_all(fd, form, strlen(form));
    read_reply(fd, r);
    lwip_close(fd);
}

static void nas(void)
{
    char target[64];
    struct reply r;
    long id;

    post_form("/smb/save", "host=192.0.2.1&user=me&password=pw", &r);
    CHECK(r.status == 200);
    id = json_number(r.body, "id");
    request("GET", "/smb/servers", "", &r);
    CHECK(r.status == 200 && strstr(r.body, "\"host\":\"192.0.2.1\"") && !strstr(r.body, "pw\""));
    snprintf(target, sizeof target, "/smb/shares?id=%ld", id);
    request("GET", target, "", &r);
    CHECK(r.status == 409 && strstr(r.body, "\"error\":\"nas_denied\""));
    CHECK(__atomic_load_n(&g_nas_negotiated, __ATOMIC_RELAXED) == 1);
}

static void client(void)
{
    struct reply r;

    request("GET", "/", "", &r);
    CHECK(r.status == 200 && strstr(r.body, "<title>tom-ps5-usbnet"));
    request("GET", "/api/info", "", &r);
    CHECK(r.status == 200 && strstr(r.body, "\"ip\":\"192.0.2.50\"") && json_number(r.body, "link_mbps") == peer_link_mbps &&
        json_number(r.body, "max_mbps") == peer_link_mbps && strstr(r.body, "\"mode\":\"usb\""));
    request("GET", "/nothing", "", &r);
    CHECK(r.status == 404);
    request("POST", "/api/exit", "Origin: http://elsewhere.example\r\n", &r);
    CHECK(r.status == 403);
    files();
    nas();
    request("POST", "/api/exit", "", &r);
    CHECK(r.status == 200);
}

int peer_main(int fd)
{
    pthread_t thread;

    g_cable = fd;
    net_heap = malloc(MEM_SIZE + 4096);
    CHECK(net_heap);
    stack_start();
    pthread_create(&thread, NULL, dhcp_server, NULL);
    pthread_create(&thread, NULL, fake_nas, NULL);
    client();
    sys_msleep(100);
    return 0;
}
