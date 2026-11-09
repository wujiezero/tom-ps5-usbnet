/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SMB_SOCK_H
#define SMB_SOCK_H
/* Forced into every libsmb2 file (-include), before any system header: the library's
 * socket calls go to src/smb_net.c, which sends each one to the console's own stack or
 * to the USB adapter's lwIP. The system headers then declare these names with libc's
 * own signatures, which smb_net.c implements. Nothing else of libsmb2 changes. */
#define socket smb_socket
#define connect smb_connect
#define close smb_close
#define fcntl smb_fcntl
#define getsockopt smb_getsockopt
#define setsockopt smb_setsockopt
#define poll smb_poll
#define readv smb_readv
#define writev smb_writev
#endif
