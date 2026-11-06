/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VENDOR_H
#define VENDOR_H
/* The RTL8159's own configuration: what Linux's r8152 driver does, without the PHY.
 * vendor.c moves aggregated bulk transfers, vendor_regs.c sets the chip up. */
#include "frame.h"
#include "rtl.h"

#define VND_MAX_RX 32                   /* bulk IN transfers that can be pending */
#define VND_MAX_QUEUE 4                 /* bulk OUT transfers that can be in flight */
#define VND_RX_BYTES (48 * 1024)        /* Linux's rx_buf_sz for this chip */
#define VND_TX_BYTES (60 * 1024)
#define VND_MIN_TX 4096
#define VND_MIN_EARLY 8                 /* KiB */
#define VND_MAX_EARLY 48
#define VND_LINUX_COALESCE 15           /* microseconds */

struct vnd_settings {
    int rx_depth, queue, tx_bytes;
};

struct vnd_hooks {
    void (*frames)(const struct frame *frames, int count);
};

/* ---- the data path ---- */
/* Opens every transfer on these endpoints of the active configuration. */
int vnd_open(int fd, unsigned bulk_in, unsigned bulk_out, const struct vnd_hooks *hooks);
int vnd_start(void);                    /* starts the receive thread */
void vnd_close(void);
void vnd_configure(const struct vnd_settings *settings);
int vnd_gone(void);
/* Room for one frame of at most MAX_FRAME bytes, or NULL if nothing could be
 * sent in time. A pointer returned here must go to vnd_tx_commit. */
uint8_t *vnd_tx_reserve(void);
void vnd_tx_commit(size_t len);
/* Control requests and transfers never run at the same time. */
void vnd_lock(void);
void vnd_unlock(void);

/* ---- the chip ---- */
void vnd_regs_attach(int fd);
/* Whether the chip's address is `mac`. 1: yes, 0: no, -1: unreadable. */
int vnd_regs_address_matches(const uint8_t mac[6]);
/* The driver's bring-up without PHY, firmware or power writes. Stops at the first
 * failed request. */
int vnd_regs_bring_up(const uint8_t mac[6]);
/* The filter comes last, once bulk IN is pending. */
int vnd_regs_open_filter(void);
/* Stops the MAC. Writes go only where bring-up already wrote. */
void vnd_regs_stop(void);
/* PLA_PHYSTATUS; 0 if it could not be read. */
uint16_t vnd_regs_phy_status(void);
int vnd_regs_mark_link(int up);
#endif
