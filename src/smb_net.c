/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See smb_net.h and smb_sock.h. libsmb2 only makes TCP clients: a socket, a
 * non-blocking connect, SO_ERROR, TCP_NODELAY and SO_LINGER, poll on one socket, readv
 * and writev, close. Those are what lwIP is given; anything else on an lwIP socket is
 * refused with EINVAL. The console's own stack gets every call as it came. */
#include "smb_net.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

static int g_lwip;

void smb_net_use_lwip(int on)
{
    __atomic_store_n(&g_lwip, on, __ATOMIC_RELAXED);
}

int smb_net_lwip(void)
{
    return __atomic_load_n(&g_lwip, __ATOMIC_RELAXED);
}

static int on_lwip(int fd)
{
    return fd >= SMB_LWIP_BASE;
}

static int refuse(void)
{
    errno = EINVAL;
    return -1;
}

int smb_socket(int domain, int type, int protocol)
{
    int fd;

    if (!smb_net_lwip()) return socket(domain, type, protocol);
    if (domain != AF_INET || type != SOCK_STREAM) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    fd = smb_lwip_socket();
    return fd < 0 ? -1 : fd + SMB_LWIP_BASE;
}

int smb_connect(int fd, const struct sockaddr *to, socklen_t length)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *)to;

    if (!on_lwip(fd)) return connect(fd, to, length);
    if (length < (socklen_t)sizeof *in || to->sa_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return smb_lwip_connect(fd - SMB_LWIP_BASE, in->sin_addr.s_addr, in->sin_port);
}

int smb_close(int fd)
{
    return on_lwip(fd) ? smb_lwip_close(fd - SMB_LWIP_BASE) : close(fd);
}

int smb_fcntl(int fd, int command, ...)
{
    va_list list;
    int argument;

    va_start(list, command);
    argument = command == F_SETFL || command == F_SETFD ? va_arg(list, int) : 0;
    va_end(list);
    if (!on_lwip(fd)) return command == F_SETFL || command == F_SETFD ? fcntl(fd, command, argument) : fcntl(fd, command);
    /* What libsmb2 asks: read the flags, then set them with O_NONBLOCK added. */
    if (command == F_GETFL) return 0;
    if (command == F_SETFL) return smb_lwip_nonblocking(fd - SMB_LWIP_BASE, (argument & O_NONBLOCK) != 0);
    return refuse();
}

int smb_getsockopt(int fd, int level, int name, void *value, socklen_t *length)
{
    if (!on_lwip(fd)) return getsockopt(fd, level, name, value, length);
    if (level != SOL_SOCKET || name != SO_ERROR || !length || *length < (socklen_t)sizeof(int)) return refuse();
    *length = sizeof(int);
    return smb_lwip_error(fd - SMB_LWIP_BASE, value);
}

int smb_setsockopt(int fd, int level, int name, const void *value, socklen_t length)
{
    if (!on_lwip(fd)) return setsockopt(fd, level, name, value, length);
    if (level == IPPROTO_TCP && name == TCP_NODELAY && length >= (socklen_t)sizeof(int))
        return smb_lwip_nodelay(fd - SMB_LWIP_BASE, *(const int *)value);
    if (level == SOL_SOCKET && name == SO_LINGER && length >= (socklen_t)sizeof(struct linger)) {
        const struct linger *l = value;

        return smb_lwip_linger(fd - SMB_LWIP_BASE, l->l_onoff, l->l_linger);
    }
    if (level == SOL_SOCKET && name == SO_REUSEADDR) return 0;   /* a client's: nothing to do */
    return refuse();
}

int smb_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    int got;

    if (count != 1 || !on_lwip(fds[0].fd)) {
        nfds_t i;

        for (i = 0; i < count; i++)
            if (on_lwip(fds[i].fd)) return refuse();
        return poll(fds, count, timeout);
    }
    got = smb_lwip_poll(fds[0].fd - SMB_LWIP_BASE, (fds[0].events & POLLIN ? SMB_LWIP_IN : 0) | (fds[0].events & POLLOUT ? SMB_LWIP_OUT : 0), timeout);
    fds[0].revents = 0;
    if (got <= 0) return got;
    if (got & SMB_LWIP_IN) fds[0].revents |= POLLIN;
    if (got & SMB_LWIP_OUT) fds[0].revents |= POLLOUT;
    if (got & SMB_LWIP_ERR) fds[0].revents |= POLLERR;
    return 1;
}

ssize_t smb_readv(int fd, const struct iovec *iov, int count)
{
    return on_lwip(fd) ? (ssize_t)smb_lwip_readv(fd - SMB_LWIP_BASE, iov, count) : readv(fd, iov, count);
}

ssize_t smb_writev(int fd, const struct iovec *iov, int count)
{
    return on_lwip(fd) ? (ssize_t)smb_lwip_writev(fd - SMB_LWIP_BASE, iov, count) : writev(fd, iov, count);
}
