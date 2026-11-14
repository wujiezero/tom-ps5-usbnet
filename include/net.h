/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NET_H
#define NET_H
/* lwIP on top of the adapter's bulk link. Kept apart from the
 * USB headers, whose socket types are not lwIP's. */
#include "frame.h"
#include "http_server.h"

struct net_lease {
    char ip[16], mask[16], gateway[16], server[16];
    const char *source;                 /* "dhcp", "link-local" or NULL */
    unsigned long lease_seconds;
};

/* Where lwIP's frames go. reserve returns room for MAX_FRAME bytes or NULL, and
 * what it returned must go to commit. */
struct link_ops {
    uint8_t *(*reserve)(void);
    void (*commit)(size_t length);
};

int net_start(const uint8_t mac[6], const struct link_ops *link);
void net_frames(const struct frame *frames, int count);
void net_link(int up);
/* Brings the interface up and starts asking for an address, with or without a link:
 * DHCP waits for one. */
int net_up(void);
/* The address the interface has now. Returns 0 if there is one. */
int net_address(struct net_lease *lease);
/* Serves until keep_going() returns 0. */
int net_serve(const struct http_backend *backend, int (*keep_going)(void));
void net_stop(void);
#endif
