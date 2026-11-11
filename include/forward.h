/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FORWARD_H
#define FORWARD_H
/* Port forwarding: the console's own services (an FTP server, an ELF loader and the
 * like) listen on the kernel's sockets, which the adapter's address never reaches,
 * since that address is lwIP's. Each rule listens on lwIP and joins every connection
 * to the same or another port on 127.0.0.1. TCP only.
 *
 * An FTP passive reply names an address and a port: on a connection whose server
 * greets with 220, 227 and 229 replies are rewritten to the adapter's address and a
 * port that lwIP chose, joined for one connection to the port the server named.
 *
 * Rules are kept in a file (forward_set_store); with none saved, the presets. The page
 * reads and saves them under /fwd/. Like the rest of the page, there is no login. */
#include "http_server.h"

#include <stddef.h>
#include <stdint.h>

#define FORWARD_MAX 32

/* Where the rules live; read on first use. */
void forward_set_store(const char *path);
/* Listens for every rule that is on, until forward_stop(). address is the adapter's,
 * dotted. Rules saved meanwhile take effect within a fraction of a second. */
int forward_start(const char *address);
void forward_stop(void);
/* Returns HTTP_NOT_MINE for anything outside /fwd/. */
int forward_route(const struct http_request *request, struct http_stream *stream);

/* Where an FTP passive reply's address or port is, for tests. The span from start to
 * end is the six numbers of a 227 reply or the port of a 229 one. */
struct forward_passive {
    size_t start, end;
    uint32_t address;                   /* network order; 0 in a 229 reply */
    uint16_t port;
    int extended;
};
int forward_find_passive(const char *line, size_t length, struct forward_passive *out);
#endif
