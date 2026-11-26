/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PKG_APPINST_H
#define PKG_APPINST_H
/* Bridge to PS5's libSceAppInstUtil.sprx for package installation by URL, progress
 * polling, and cancellation. Implemented natively on PS5 (src/pkg_appinst.c) and
 * simulated over real loopback HTTP in host tests (tests/pkg_appinst_stub.c). */
#include <stddef.h>
#include <stdint.h>

enum pkg_appinst_state {
    PKG_STATE_TRANSFERRING,
    PKG_STATE_PROMOTING,
    PKG_STATE_PLAYABLE,
    PKG_STATE_ERROR,
    PKG_STATE_UNKNOWN
};

struct pkg_appinst_status {
    enum pkg_appinst_state state;
    uint64_t downloaded;
    uint64_t total;
    uint32_t error_code;
};

/* Starts an asynchronous package install from `uri` (e.g. http://127.0.0.1:<port>/...).
 * Returns NULL on success and copies the content ID into `out_content_id`, or returns
 * a short error key ("no_install_api", "install_refused", "no_space", "no_memory")
 * and puts the raw Sony error code (if non-zero) in `*out_err`. */
const char *pkg_appinst_start(const char *uri, const char *content_name, char *out_content_id, size_t out_cap, uint32_t *out_err);

/* Queries installation status for `content_id`. Returns 0 on success, -1 on failure. */
int pkg_appinst_poll(const char *content_id, struct pkg_appinst_status *out);

/* Best-effort cancellation of an ongoing install for `content_id`. */
void pkg_appinst_cancel(const char *content_id);
#endif
