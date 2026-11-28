/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PKG_STREAM_H
#define PKG_STREAM_H
/* Loopback HTTP/1.1 Range server (127.0.0.1) backed by an SMB share file. Allows PS5's
 * installer to stream bytes over HTTP/1.1 206 Partial Content without staging on disk.
 * Owns a copy of server credentials (wiped on stop) and a small pool of SMB sessions
 * so concurrent Range connections each run safely on their own session. */
#include "smb_client.h"

#include <stddef.h>
#include <stdint.h>

struct pkg_stream;

/* Optional log callback (never receives filenames, tokens, or IP addresses). */
void pkg_stream_set_log(void (*log)(const char *line));

/* Starts listening on 127.0.0.1 (ephemeral port) backed by an SMB share file.
 * On success (returns 0), takes ownership of `initial_session` and `initial_file`;
 * on failure (returns -1), caller keeps them. */
int pkg_stream_start(const struct smb_server *server, const char *share, const char *path, struct smb_session *initial_session, void *initial_file,
    uint64_t size, int64_t mtime, struct pkg_stream **out);

/* Starts listening on 127.0.0.1 (ephemeral port) backed by on-demand browser chunk pulls. */
int pkg_stream_start_browser(uint64_t size, int64_t mtime, struct pkg_stream **out);

/* Waits up to `timeout_ms` for a needed byte range. Returns 1 with req_id/offset/len,
 * 0 on timeout (idle), or -1 when the stream is stopping. */
int pkg_stream_pull_need(struct pkg_stream *st, uint64_t *req_id, uint64_t *offset, size_t *len, int timeout_ms);

/* Supplies data for `req_id` (takes ownership of heap buffer `data` on 0; returns -1 if
 * stale/stopped, in which case caller still owns `data`). */
int pkg_stream_pull_give(struct pkg_stream *st, uint64_t req_id, uint8_t *data, size_t len);

/* Writes http://127.0.0.1:<port>/pkg/<token>/package.pkg into `out`. */
void pkg_stream_url(const struct pkg_stream *st, char *out, size_t capacity);

/* Total payload bytes written to clients so far. */
uint64_t pkg_stream_served(const struct pkg_stream *st);

/* Currently connected HTTP client count. */
int pkg_stream_active_clients(const struct pkg_stream *st);

/* Last non-recoverable SMB_* error code encountered by workers, or 0. */
int pkg_stream_last_smb_error(const struct pkg_stream *st);

/* Signals workers and any blocked pull_need() callers to wake up and stop. */
void pkg_stream_signal_stop(struct pkg_stream *st);

/* Shuts down listener and active client sockets, joins/waits for all threads,
 * closes pooled SMB sessions, wipes credentials, and frees `st`. */
void pkg_stream_stop(struct pkg_stream *st);
#endif
