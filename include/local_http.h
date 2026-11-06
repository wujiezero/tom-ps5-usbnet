/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5_LOCAL_HTTP_H
#define PS5_LOCAL_HTTP_H
#include "http_server.h"

/* The same page for the console's own browser, on 127.0.0.1 only. See local_http.c. */
int local_http_start(const struct http_backend *backend, uint16_t port);
void local_http_stop(void);
#endif
