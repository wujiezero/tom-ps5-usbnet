/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SECRET_H
#define SECRET_H
/* Saved NAS passwords, sealed with a key that only this console can make: AES-128-CCM
 * (libsmb2's) under HMAC-SHA256 of the console's OpenPSID. The ID and the key stay in
 * memory and are never written or logged. A copy of nas.txt, through the page or on a
 * drive, opens on no other console and on no computer. */
#include <stddef.h>
#include <stdint.h>

#define SECRET_MAX 127                  /* longest password, in bytes */
#define SECRET_HEX ((11 + 1 + SECRET_MAX + 16) * 2 + 1)

/* Where the console's ID comes from; tests set their own. 0 on success. */
void secret_set_source(int (*console_id)(uint8_t id[16]));
/* Whether there is a key. Without one, nothing can be sealed or opened. */
int secret_ready(void);
/* `plain` sealed to hex in `out`, bound to `context` (what it belongs to). 0 on success. */
int secret_seal(const char *context, const char *plain, char *out, size_t capacity);
/* The reverse: 0, and the text in `plain`, only if it was sealed here with this context. */
int secret_open(const char *context, const char *hex, char *plain, size_t capacity);
#endif
