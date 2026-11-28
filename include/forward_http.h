/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TOM_FORWARD_HTTP_H
#define TOM_FORWARD_HTTP_H
#include <stddef.h>
#include <stdint.h>

#define FORWARD_HTTP_HEAD 16384
/* Translates one validated HTTP/1.1 request. HEAD includes its blank line only. */
int forward_http_head(const char *head, size_t length, const char *external,
                      const char *upstream, char *output, size_t capacity, uint64_t *body);
#endif
