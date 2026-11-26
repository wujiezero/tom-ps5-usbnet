/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PKG_INSTALL_H
#define PKG_INSTALL_H
/* Streaming installation of a .pkg/.fpkg stored on a NAS (SMB) share.
 * Validates the package header over SMB, exposes a loopback HTTP/1.1 Range
 * endpoint (pkg_stream.h), invokes Sony's libSceAppInstUtil (pkg_appinst.h), and
 * tracks progress/cancellation through files.c's background job slot. */
#include "http_server.h"
#include "smb_client.h"

/* Optional log callback for streaming install events (never receives filenames or IPs). */
void pkg_install_set_log(void (*log)(const char *line));

/* Returns 1 if `path` ends with ".pkg" or ".fpkg" (case-insensitive) with a non-empty stem. */
int pkg_install_is_pkg_name(const char *path);

/* Starts an async NAS streaming install job on files.c's job slot and replies on `stream`. */
int pkg_install_smb(const struct smb_server *server, const char *share, const char *path, const char *client, struct http_stream *stream);
#endif
