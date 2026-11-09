/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5_LOCAL_HTTP_H
#define PS5_LOCAL_HTTP_H
#include "http_server.h"

/* The same page on the kernel's sockets: for the console's own browser on 127.0.0.1
 * only, or with `every_address` on all of the console's. See local_http.c. */
int local_http_start(const struct http_backend *backend, uint16_t port, int every_address);
void local_http_stop(void);
/* The listener stopped on an error, as after rest mode: stop it and start it again. */
int local_http_broken(void);
#endif
