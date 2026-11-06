/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TILE_H
#define TILE_H
/* The home-screen icon, and the rights the running ELF needs for it. See tile.c. */
#include <stddef.h>
#include <stdint.h>

#define TILE_TITLE_ID "PSUN00001"
#define TILE_PORT 9190               /* ps5upload's own page is on 9180 */

/* Root, every capability, out of the sandbox: what ps5upload does at start.
 * -1 if the kernel could not be reached. Logged either way. */
int tile_elevate(void);
/* Puts the icon on the home screen, or binds it again after a reboot. Runs on a thread. */
void tile_install(void);
/* At the start of a run: binds it again if an earlier run registered it, and otherwise
 * leaves it to the page's button, so that a failure cannot cost the run. */
void tile_resume(void);
/* Takes it off again, waits for that, and returns 0 if it is gone. */
int tile_remove(void);
/* {"state":"...","error":"..."} */
void tile_status(char *out, size_t capacity);
#endif
