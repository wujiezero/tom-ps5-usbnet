/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See p3_net.h. The receive thread holds the core lock while it feeds frames in,
 * which is what tcpip_input would do one frame at a time with
 * LWIP_TCPIP_CORE_LOCKING_INPUT, without taking the lock per frame. */
#include "net.h"

#include "lwip/autoip.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/prot/dhcp.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "netif/ethernet.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_PORT 80
#define MAX_WORKERS 8
#define IO_TIMEOUT_S 30
#define WORKER_STACK (1024 * 1024)
#define WORKER_WAIT_MS 3000
#define RELEASE_FLUSH_MS 200

unsigned char *net_heap;

struct worker {
    int fd;
    const struct http_backend *backend;
};

static struct netif g_netif;
static struct link_ops g_link;
static uint8_t g_mac[6];
static int g_up;
static int g_workers;
static pthread_mutex_t g_workers_lock = PTHREAD_MUTEX_INITIALIZER;

static err_t link_output(struct netif *netif, struct pbuf *packet)
{
    uint8_t *slot;

    (void)netif;
    if (packet->tot_len > MAX_FRAME) return ERR_IF;
    slot = g_link.reserve();
    if (!slot) return ERR_IF;
    pbuf_copy_partial(packet, slot, packet->tot_len, 0);
    g_link.commit(packet->tot_len);
    return ERR_OK;
}

static err_t interface_init(struct netif *netif)
{
    netif->name[0] = 'u';
    netif->name[1] = 'e';
    netif->output = etharp_output;
    netif->linkoutput = link_output;
    netif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(netif->hwaddr, g_mac, ETH_HWADDR_LEN);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

static void core_ready(void *semaphore)
{
    sys_sem_signal(semaphore);
}

int net_start(const uint8_t mac[6], const struct link_ops *link)
{
    sys_sem_t ready;
    struct netif *added;

    memcpy(g_mac, mac, sizeof g_mac);
    g_link = *link;
    net_heap = malloc(MEM_SIZE + 4096);
    if (!net_heap || sys_sem_new(&ready, 0) != ERR_OK) return -1;
    tcpip_init(core_ready, &ready);
    sys_sem_wait(&ready);
    sys_sem_free(&ready);
    LOCK_TCPIP_CORE();
    added = netif_add(&g_netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, interface_init, ethernet_input);
    if (added) netif_set_default(added);
    UNLOCK_TCPIP_CORE();
    return added ? 0 : -1;
}

void net_frames(const struct frame *frames, int count)
{
    int i;

    LOCK_TCPIP_CORE();
    for (i = 0; i < count && g_up; i++) {
        struct pbuf *packet = pbuf_alloc(PBUF_RAW, (u16_t)frames[i].len, PBUF_POOL);

        if (!packet) continue;          /* counted by lwIP as a pool error */
        if (pbuf_take(packet, frames[i].data, (u16_t)frames[i].len) != ERR_OK ||
                g_netif.input(packet, &g_netif) != ERR_OK)
            pbuf_free(packet);
    }
    UNLOCK_TCPIP_CORE();
}

void net_link(int up)
{
    LOCK_TCPIP_CORE();
    if (up) netif_set_link_up(&g_netif);
    else netif_set_link_down(&g_netif);
    UNLOCK_TCPIP_CORE();
}

int net_acquire(long timeout_ms, struct net_lease *lease)
{
    u32_t start = sys_now();
    int done = 0;

    LOCK_TCPIP_CORE();
    g_up = 1;
    netif_set_up(&g_netif);
    if (dhcp_start(&g_netif) != ERR_OK) done = -1;
    UNLOCK_TCPIP_CORE();
    memset(lease, 0, sizeof *lease);
    while (!done && (long)(sys_now() - start) < timeout_ms) {
        LOCK_TCPIP_CORE();
        if (dhcp_supplied_address(&g_netif)) {
            const struct dhcp *dhcp = netif_dhcp_data(&g_netif);

            lease->source = "dhcp";
            lease->lease_seconds = dhcp->offered_t0_lease;
            ip4addr_ntoa_r(ip_2_ip4(&dhcp->server_ip_addr), lease->server, sizeof lease->server);
        } else if (autoip_supplied_address(&g_netif)) {
            lease->source = "link-local";
        }
        if (lease->source) {
            ip4addr_ntoa_r(netif_ip4_addr(&g_netif), lease->ip, sizeof lease->ip);
            ip4addr_ntoa_r(netif_ip4_netmask(&g_netif), lease->mask, sizeof lease->mask);
            ip4addr_ntoa_r(netif_ip4_gw(&g_netif), lease->gateway, sizeof lease->gateway);
            done = 1;
        }
        UNLOCK_TCPIP_CORE();
        sys_msleep(50);
    }
    return done == 1 ? 0 : -1;
}

static ssize_t stream_read(void *context, void *data, size_t length)
{
    return lwip_read(*(int *)context, data, length);
}

static ssize_t stream_write(void *context, const void *data, size_t length)
{
    return lwip_write(*(int *)context, data, length);
}

static void *worker_main(void *opaque)
{
    struct worker work = *(struct worker *)opaque;
    struct http_stream stream = {&work.fd, stream_read, stream_write};

    free(opaque);
    http_serve(work.backend, &stream);
    lwip_close(work.fd);
    pthread_mutex_lock(&g_workers_lock);
    g_workers--;
    pthread_mutex_unlock(&g_workers_lock);
    return NULL;
}

static int start_worker(const struct http_backend *backend, int fd)
{
    struct worker *work;
    pthread_attr_t attr;
    pthread_t thread;
    int result;

    pthread_mutex_lock(&g_workers_lock);
    result = g_workers < MAX_WORKERS ? 0 : -1;
    if (!result) g_workers++;
    pthread_mutex_unlock(&g_workers_lock);
    if (result) return -1;
    work = malloc(sizeof *work);
    if (work) {
        work->fd = fd;
        work->backend = backend;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, WORKER_STACK);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        result = pthread_create(&thread, &attr, worker_main, work);
        pthread_attr_destroy(&attr);
        if (!result) return 0;
        free(work);
    }
    pthread_mutex_lock(&g_workers_lock);
    g_workers--;
    pthread_mutex_unlock(&g_workers_lock);
    return -1;
}

static void connection_options(int fd)
{
    struct timeval timeout = {IO_TIMEOUT_S, 0};
    int on = 1;

    lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    lwip_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}

int net_serve(const struct http_backend *backend, int (*keep_going)(void))
{
    struct sockaddr_in address;
    int listener = lwip_socket(AF_INET, SOCK_STREAM, 0), nonblocking = 1, workers;
    u32_t waited;

    memset(&address, 0, sizeof address);
    address.sin_len = sizeof address;
    address.sin_family = AF_INET;
    address.sin_port = lwip_htons(HTTP_PORT);
    if (listener < 0 || lwip_bind(listener, (struct sockaddr *)&address, sizeof address) < 0 ||
            lwip_listen(listener, 16) < 0 || lwip_ioctl(listener, FIONBIO, &nonblocking) < 0) {
        if (listener >= 0) lwip_close(listener);
        return -1;
    }
    while (keep_going()) {
        struct sockaddr_in client;
        socklen_t size = sizeof client;
        int fd = lwip_accept(listener, (struct sockaddr *)&client, &size);

        if (fd < 0) {
            sys_msleep(10);
            continue;
        }
        connection_options(fd);
        if (start_worker(backend, fd) != 0) lwip_close(fd);
    }
    lwip_close(listener);
    for (waited = sys_now(); sys_now() - waited < WORKER_WAIT_MS; sys_msleep(10)) {
        pthread_mutex_lock(&g_workers_lock);
        workers = g_workers;
        pthread_mutex_unlock(&g_workers_lock);
        if (!workers) break;
    }
    return 0;
}

void net_stop(void)
{
    LOCK_TCPIP_CORE();
    dhcp_release_and_stop(&g_netif);
    autoip_stop(&g_netif);
    UNLOCK_TCPIP_CORE();
    sys_msleep(RELEASE_FLUSH_MS);
    LOCK_TCPIP_CORE();
    g_up = 0;
    netif_set_down(&g_netif);
    UNLOCK_TCPIP_CORE();
}
