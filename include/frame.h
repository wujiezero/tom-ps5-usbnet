/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FRAME_H
#define FRAME_H
#include <stddef.h>
#include <stdint.h>

#define MAX_FRAME 1514

struct frame {
    const uint8_t *data;
    size_t len;
};
#endif
