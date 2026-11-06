/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ADAPTER_H
#define ADAPTER_H
#include "ncm.h"

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#include <stddef.h>
#include <stdint.h>

#define ADAPTER_WAIT_SECONDS 60

extern int adapter_gone;

struct adapter {
    char path[32];
    int fd;
    int original_index, ecm_index, active_index;
    unsigned data_iface;                /* what the end of the run puts back */
    struct ncm_function ecm;
};

/* Finds the adapter, runs `run` with it open, puts it back as it was found. Returns
 * what main() should return. */
int adapter_main(void (*run)(struct adapter *adapter));
/* A device-to-host request. Returns the bytes read, or -1. */
int adapter_control_in(int fd, const uint8_t setup[8], uint8_t *data);
int adapter_read_config(struct adapter *adapter, int index, uint8_t *data, size_t capacity);
int adapter_select_config(struct adapter *adapter, int index);
int adapter_driver_active(struct adapter *adapter);
/* Enables the ECM configuration's data interface, then leaves it. */
int adapter_pass_through_ecm(struct adapter *adapter);
/* The adapter's own address. Never to be logged. */
int adapter_read_mac(struct adapter *adapter, uint8_t mac[6]);
#endif
