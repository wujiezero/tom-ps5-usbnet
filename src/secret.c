/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See secret.h. A sealed text is a fresh 11-byte nonce, then the text padded to a fixed
 * length (one length byte and SECRET_MAX bytes, so its length does not show), then a
 * 16-byte tag, all in hex. */
#include "secret.h"

#include "aes128ccm.h"
#include "sha.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NONCE 11
#define BODY (1 + SECRET_MAX)
#define TAG 16
#define SEALED (NONCE + BODY + TAG)
#define LABEL "tom-ps5-usbnet nas.txt v2"

typedef int (*id_fn)(void *id);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int (*g_source)(uint8_t id[16]);
static int g_state;                     /* 0: not tried, 1: ready, -1: no key */
static uint8_t g_key[16];

/* sceKernelGetOpenPsId, resolved at run time as log.c does with notifications. */
static int openpsid(uint8_t id[16])
{
    id_fn get = (id_fn)dlsym(RTLD_DEFAULT, "sceKernelGetOpenPsId");

    return get ? get(id) : -1;
}

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;

    while (n--) *v++ = 0;
}

void secret_set_source(int (*console_id)(uint8_t id[16]))
{
    pthread_mutex_lock(&g_lock);
    g_source = console_id;
    g_state = 0;
    wipe(g_key, sizeof g_key);
    pthread_mutex_unlock(&g_lock);
}

int secret_ready(void)
{
    uint8_t id[16], digest[USHAMaxHashSize];
    static const uint8_t zero[16];
    int state;

    pthread_mutex_lock(&g_lock);
    if (!g_state) {
        memset(id, 0, sizeof id);
        g_state = -1;
        /* An all-zero ID would give every such console the same key. */
        if ((g_source ? g_source : openpsid)(id) == 0 && memcmp(id, zero, sizeof id) &&
                hmac(SHA256, (const unsigned char *)LABEL, strlen(LABEL), id, sizeof id, digest) == 0) {
            memcpy(g_key, digest, sizeof g_key);
            g_state = 1;
        }
        wipe(id, sizeof id);
        wipe(digest, sizeof digest);
    }
    state = g_state;
    pthread_mutex_unlock(&g_lock);
    return state == 1;
}

int secret_seal(const char *context, const char *plain, char *out, size_t capacity)
{
    uint8_t sealed[SEALED], key[16];
    size_t length = strlen(plain), i;

    if (length > SECRET_MAX || capacity < SEALED * 2 + 1 || !secret_ready()) return -1;
    pthread_mutex_lock(&g_lock);
    memcpy(key, g_key, sizeof key);
    pthread_mutex_unlock(&g_lock);
    arc4random_buf(sealed, NONCE);
    memset(sealed + NONCE, 0, BODY);
    sealed[NONCE] = (uint8_t)length;
    memcpy(sealed + NONCE + 1, plain, length);
    aes128ccm_encrypt(key, sealed, NONCE, (unsigned char *)context, strlen(context), sealed + NONCE, BODY, sealed + NONCE + BODY, TAG);
    for (i = 0; i < SEALED; i++) snprintf(out + 2 * i, 3, "%02x", sealed[i]);
    wipe(key, sizeof key);
    return 0;
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int secret_open(const char *context, const char *hex, char *plain, size_t capacity)
{
    uint8_t sealed[SEALED], key[16];
    size_t i;
    int result = -1;

    if (strlen(hex) != SEALED * 2 || !secret_ready()) return -1;
    for (i = 0; i < SEALED; i++) {
        int high = nibble(hex[2 * i]), low = nibble(hex[2 * i + 1]);

        if (high < 0 || low < 0) return -1;
        sealed[i] = (uint8_t)(high << 4 | low);
    }
    pthread_mutex_lock(&g_lock);
    memcpy(key, g_key, sizeof key);
    pthread_mutex_unlock(&g_lock);
    if (aes128ccm_decrypt(key, sealed, NONCE, (unsigned char *)context, strlen(context), sealed + NONCE, BODY, sealed + NONCE + BODY, TAG) == 0 &&
            sealed[NONCE] <= SECRET_MAX && sealed[NONCE] < capacity && !memchr(sealed + NONCE + 1, 0, sealed[NONCE])) {
        memcpy(plain, sealed + NONCE + 1, sealed[NONCE]);
        plain[sealed[NONCE]] = 0;
        result = 0;
    }
    wipe(sealed, sizeof sealed);
    wipe(key, sizeof key);
    return result;
}
