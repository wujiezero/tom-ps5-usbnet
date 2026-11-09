/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What the host builds use in place of tile.c, which needs the console's kernel and
 * installer. Records what was asked of it. */
#include "tile.h"
#include "crash.h"

#include <stdio.h>

int tile_calls[4];

int tile_elevate(void) { tile_calls[0]++; return 0; }
void tile_install(void) { tile_calls[1]++; }
void tile_resume(void) { tile_calls[1]++; }
int tile_remove(void) { tile_calls[2]++; return 0; }
void tile_status(char *out, size_t capacity) { snprintf(out, capacity, "{\"state\":\"installed\",\"error\":\"\"}"); }
void crash_log_start(void) {}
void crash_stage(const char *what) { (void)what; }
