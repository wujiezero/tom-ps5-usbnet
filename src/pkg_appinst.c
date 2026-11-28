/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native PS5 installer bridge using libSceAppInstUtil.sprx (already linked in
 * DT_NEEDED after libSceIpmi, see scripts/build_ps5.py and src/tile.c).
 *
 * Uses sceAppInstUtilInstallByPackage() with a local HTTP loopback URL so PS5's
 * installer pulls bytes directly through our SMB stream without touching disk first.
 * Never calls sceAppInstUtilTerminate() because tile.c shares the same process. */
#include "pkg_appinst.h"
#include "log.h"

#include <dlfcn.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APPINST_META_HEAP 256
#define APPINST_PKG_INFO_HEAP 4096
#define APPINST_PLAYGO_HEAP 32768
#define APPINST_STATUS_HEAP 4096

#define APPINST_CONTENT_ID_SIZE 0x30
#define APPINST_PLAYGO_SID_SIZE 3
#define APPINST_LANG_SIZE 8
#define APPINST_NUM_LANGS 30
#define APPINST_NUM_IDS 64

#define APPINST_ERR_NO_SPACE_1 0x80990005u
#define APPINST_ERR_NO_SPACE_2 0x8099000fu
#define APPINST_ERR_CANCELED 0x80900013u

struct appinst_meta_info {
    const char *uri;
    const char *ex_uri;
    const char *playgo_scenario_id;
    const char *content_id;
    const char *content_name;
    const char *icon_url;
    uint64_t reserved[4];
};

struct appinst_pkg_info {
    char content_id[APPINST_CONTENT_ID_SIZE];
    int content_type;
    int content_platform;
    uint8_t pad[128];
};

struct appinst_playgo_info {
    char languages[APPINST_NUM_LANGS][APPINST_LANG_SIZE];
    char playgo_scenario_ids[APPINST_NUM_IDS][APPINST_PLAYGO_SID_SIZE];
    char content_ids[APPINST_NUM_IDS][APPINST_CONTENT_ID_SIZE];
    uint8_t unknown[12880];
};

struct appinst_error_info {
    int32_t error_code;
    int32_t version;
    char description[512];
    char type[9];
};

struct appinst_status_raw {
    char status[16];
    char src_type[8];
    uint32_t remain_time;
    uint64_t downloaded_size;
    uint64_t initial_chunk_size;
    uint64_t total_size;
    uint32_t promote_progress;
    struct appinst_error_info error_info;
    int32_t local_copy_percent;
    uint8_t is_copy_only;
    uint8_t pad[64];
};

typedef int (*appinst_init_fn)(void);
typedef int (*appinst_install_pkg_fn)(const struct appinst_meta_info *meta, struct appinst_pkg_info *pkg_info, struct appinst_playgo_info *playgo);
typedef int (*appinst_get_status_fn)(const char *content_id, struct appinst_status_raw *status);
typedef int (*appinst_cancel_fn)(const char *content_id);

static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized;

static void ensure_init(void)
{
    appinst_init_fn init_fn;

    pthread_mutex_lock(&g_init_lock);
    if (!g_initialized) {
        init_fn = (appinst_init_fn)dlsym(RTLD_DEFAULT, "sceAppInstUtilInitialize");
        if (init_fn) {
            int rc = init_fn();

            say("pkg: initialize returned %#x", (unsigned)rc);
            g_initialized = 1;
        }
    }
    pthread_mutex_unlock(&g_init_lock);
}

const char *pkg_appinst_start(const char *uri, const char *content_name, char *out_content_id, size_t out_cap, uint32_t *out_err)
{
    appinst_install_pkg_fn install_fn;
    struct appinst_meta_info *meta;
    struct appinst_pkg_info *pkg_info;
    struct appinst_playgo_info *playgo;
    int rc;

    if (out_content_id && out_cap) out_content_id[0] = 0;
    if (out_err) *out_err = 0;
    install_fn = (appinst_install_pkg_fn)dlsym(RTLD_DEFAULT, "sceAppInstUtilInstallByPackage");
    if (!install_fn) {
        say("pkg: sceAppInstUtilInstallByPackage missing");
        return "no_install_api";
    }
    ensure_init();
    meta = calloc(1, APPINST_META_HEAP);
    pkg_info = calloc(1, APPINST_PKG_INFO_HEAP);
    playgo = calloc(1, APPINST_PLAYGO_HEAP);
    if (!meta || !pkg_info || !playgo) {
        free(meta);
        free(pkg_info);
        free(playgo);
        return "no_memory";
    }
    meta->uri = uri ? uri : "";
    if (content_name && content_name[0]) meta->content_name = content_name;
    rc = install_fn(meta, pkg_info, playgo);
    say("pkg: install (name=%s) returned %#x", (content_name && content_name[0]) ? "set" : "none", (unsigned)rc);
    if (rc == 0) {
        pkg_info->content_id[APPINST_CONTENT_ID_SIZE - 1] = 0;
        say("pkg: cid_len=%zu", strlen(pkg_info->content_id));
    }
    checkpoint();
    if (rc != 0) {
        uint32_t code = (uint32_t)rc;

        free(meta);
        free(pkg_info);
        free(playgo);
        if (out_err) *out_err = code;
        if (code == APPINST_ERR_NO_SPACE_1 || code == APPINST_ERR_NO_SPACE_2) return "no_space";
        if (code == APPINST_ERR_CANCELED) return "canceled";
        return "install_refused";
    }
    if (out_content_id && out_cap) snprintf(out_content_id, out_cap, "%s", pkg_info->content_id);
    free(meta);
    free(pkg_info);
    free(playgo);
    return NULL;
}

int pkg_appinst_poll(const char *content_id, struct pkg_appinst_status *out)
{
    static int logged_first;
    appinst_get_status_fn status_fn;
    struct appinst_status_raw *raw;
    int rc;

    if (!content_id || !content_id[0] || !out) return -1;
    status_fn = (appinst_get_status_fn)dlsym(RTLD_DEFAULT, "sceAppInstUtilGetInstallStatus");
    if (!logged_first) {
        say("pkg: poll status_fn %s", status_fn ? "found" : "missing");
        checkpoint();
    }
    if (!status_fn) {
        logged_first = 1;
        return -1;
    }
    raw = calloc(1, APPINST_STATUS_HEAP);
    if (!raw) return -1;
    rc = status_fn(content_id, raw);
    if (!logged_first) {
        logged_first = 1;
        say("pkg: poll rc=%#x dl=%" PRIu64 " total=%" PRIu64 " err=%#x", (unsigned)rc, raw->downloaded_size, raw->total_size,
            (unsigned)raw->error_info.error_code);
        checkpoint();
    }
    if (rc != 0) {
        free(raw);
        return -1;
    }
    raw->status[sizeof raw->status - 1] = 0;
    out->downloaded = raw->downloaded_size;
    out->total = raw->total_size;
    out->error_code = (uint32_t)raw->error_info.error_code;
    if (raw->error_info.error_code != 0 || !strcmp(raw->status, "error")) out->state = PKG_STATE_ERROR;
    else if (!strcmp(raw->status, "playable")) out->state = PKG_STATE_PLAYABLE;
    else if (!strcmp(raw->status, "promoting")) out->state = PKG_STATE_PROMOTING;
    else if (!strcmp(raw->status, "transferring")) out->state = PKG_STATE_TRANSFERRING;
    else out->state = PKG_STATE_UNKNOWN;
    free(raw);
    return 0;
}

void pkg_appinst_cancel(const char *content_id)
{
    appinst_cancel_fn cancel_fn;

    if (!content_id || !content_id[0]) return;
    cancel_fn = (appinst_cancel_fn)dlsym(RTLD_DEFAULT, "sceAppInstUtilCancelInstall");
    if (cancel_fn) {
        int rc = cancel_fn(content_id);

        say("pkg: cancel returned %#x", (unsigned)rc);
    }
}
