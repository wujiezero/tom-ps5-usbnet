/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SMB_NET_H
#define SMB_NET_H
#include <stdint.h>
/* Which stack the next SMB connection uses (see include/smb_sock.h). Sockets made on
 * lwIP carry numbers from SMB_LWIP_BASE up, so every later call on them finds its way
 * back there whatever the setting is by then. */
#define SMB_LWIP_BASE 0x100000

void smb_net_use_lwip(int on);
int smb_net_lwip(void);

/* lwIP's side, in src/smb_lwip.c: only lwIP's headers are seen there. Numbers are
 * lwIP's own sockets; addresses and ports in network order; -1 with errno set. */
#define SMB_LWIP_IN 1
#define SMB_LWIP_OUT 2
#define SMB_LWIP_ERR 4
int smb_lwip_socket(void);
int smb_lwip_connect(int fd, uint32_t address, uint16_t port);
int smb_lwip_nonblocking(int fd, int on);
int smb_lwip_error(int fd, int *error);
int smb_lwip_nodelay(int fd, int on);
int smb_lwip_linger(int fd, int on, int seconds);
int smb_lwip_close(int fd);
long smb_lwip_readv(int fd, const void *iov, int count);
long smb_lwip_writev(int fd, const void *iov, int count);
/* One socket: `want` and the result in SMB_LWIP_* bits. 0 on a timeout. */
int smb_lwip_poll(int fd, int want, int timeout_ms);
#endif
