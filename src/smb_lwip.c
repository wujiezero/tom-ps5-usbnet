/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See smb_net.h. lwIP's socket names and constants differ from libc's (SOL_SOCKET,
 * the poll bits, O_NONBLOCK), so they are only ever spelled here. */
#include <stdint.h>
#include <string.h>

#include "smb_net.h"

#include "lwip/sockets.h"
#include "lwip/tcp.h"

int smb_lwip_socket(void)
{
    return lwip_socket(AF_INET, SOCK_STREAM, 0);
}

int smb_lwip_connect(int fd, uint32_t address, uint16_t port)
{
    struct sockaddr_in to;

    memset(&to, 0, sizeof to);
    to.sin_len = sizeof to;
    to.sin_family = AF_INET;
    to.sin_port = port;
    to.sin_addr.s_addr = address;
    return lwip_connect(fd, (struct sockaddr *)&to, sizeof to);
}

int smb_lwip_nonblocking(int fd, int on)
{
    u32_t value = on ? 1 : 0;

    return lwip_ioctl(fd, (long)FIONBIO, &value);
}

int smb_lwip_error(int fd, int *error)
{
    socklen_t length = sizeof *error;

    return lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, error, &length);
}

int smb_lwip_nodelay(int fd, int on)
{
    return lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}

int smb_lwip_linger(int fd, int on, int seconds)
{
    struct linger value = {on, seconds};

    return lwip_setsockopt(fd, SOL_SOCKET, SO_LINGER, &value, sizeof value);
}

int smb_lwip_close(int fd)
{
    return lwip_close(fd);
}

long smb_lwip_readv(int fd, const void *iov, int count)
{
    return (long)lwip_readv(fd, (const struct iovec *)iov, count);
}

long smb_lwip_writev(int fd, const void *iov, int count)
{
    return (long)lwip_writev(fd, (const struct iovec *)iov, count);
}

int smb_lwip_poll(int fd, int want, int timeout_ms)
{
    struct pollfd p;
    int result, got = 0;

    p.fd = fd;
    p.events = (short)((want & SMB_LWIP_IN ? POLLIN : 0) | (want & SMB_LWIP_OUT ? POLLOUT : 0));
    p.revents = 0;
    result = lwip_poll(&p, 1, timeout_ms);
    if (result <= 0) return result;
    if (p.revents & POLLIN) got |= SMB_LWIP_IN;
    if (p.revents & POLLOUT) got |= SMB_LWIP_OUT;
    if (p.revents & (POLLERR | POLLNVAL)) got |= SMB_LWIP_ERR;
    return got ? got : SMB_LWIP_ERR;
}
