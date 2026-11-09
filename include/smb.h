/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SMB_H
#define SMB_H
/* The page's NAS routes, under /smb/: saved servers, their shares and folders, and a
 * copy from a share to the console's storage, which runs as the file manager's one
 * job. Passwords are kept on the console (smb_set_store) and never sent back. */
#include "http_server.h"

/* Where the saved servers live; read on first use. */
void smb_set_store(const char *path);
/* Returns HTTP_NOT_MINE for anything outside /smb/. */
int smb_route(const struct http_request *request, struct http_stream *stream);
#endif
