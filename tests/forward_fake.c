/* SPDX-License-Identifier: GPL-3.0-or-later */
/* forward_lwip.h on the host's own sockets, for tests and the page server: a rule's
 * port P listens on 127.0.0.1 at P + FAKE_OFFSET, so that it never meets the service
 * it leads to. A port lwIP would choose is the host's, as it is. */
#include "forward_lwip.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#define FAKE_OFFSET 30000
#define FAKE_FDS 4096

static int g_offset[FAKE_FDS];

int fwd_lwip_listen(uint16_t port, int backlog)
{
    struct sockaddr_in address;
    int fd = socket(AF_INET, SOCK_STREAM, 0), on = 1, saved;

    if (fd < 0 || fd >= FAKE_FDS) {
        if (fd >= 0) close(fd);
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons(port ? (uint16_t)(port + FAKE_OFFSET) : 0);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&address, sizeof address) == 0 && listen(fd, backlog) == 0 &&
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0) {
        g_offset[fd] = port ? FAKE_OFFSET : 0;
        return fd;
    }
    saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

int fwd_lwip_port(int fd)
{
    struct sockaddr_in address;
    socklen_t length = sizeof address;

    if (getsockname(fd, (struct sockaddr *)&address, &length) != 0) return -1;
    return ntohs(address.sin_port) - g_offset[fd];
}

int fwd_lwip_wait(const int *fds, int count, int timeout_ms, int *ready)
{
    struct pollfd wait[64];
    int i, result;

    for (i = 0; i < count && i < 64; i++) {
        wait[i].fd = fds[i];
        wait[i].events = POLLIN;
        wait[i].revents = 0;
        ready[i] = 0;
    }
    result = poll(wait, (nfds_t)i, timeout_ms);
    for (i = 0; result > 0 && i < count && i < 64; i++) ready[i] = (wait[i].revents & (POLLIN | POLLERR)) != 0;
    return result;
}

int fwd_lwip_accept(int listener, uint32_t *peer, int timeout_ms)
{
    struct sockaddr_in from;
    socklen_t length = sizeof from;
    struct timeval timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int fd = accept(listener, (struct sockaddr *)&from, &length), on = 1;

    if (fd < 0) return -1;
    *peer = from.sin_addr.s_addr;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    return fd;
}

long fwd_lwip_read(int fd, void *data, size_t length)
{
    ssize_t n = read(fd, data, length);

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return FWD_LWIP_TIMEOUT;
    return (long)n;
}

long fwd_lwip_write(int fd, const void *data, size_t length)
{
    ssize_t n = write(fd, data, length);

    if (n <= 0 && (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return FWD_LWIP_TIMEOUT;
    return (long)n;
}

void fwd_lwip_shutdown(int fd)
{
    shutdown(fd, SHUT_WR);
}

void fwd_lwip_close(int fd, int reset)
{
    if (reset) {
        struct linger now = {1, 0};

        setsockopt(fd, SOL_SOCKET, SO_LINGER, &now, sizeof now);
    }
    close(fd);
}
