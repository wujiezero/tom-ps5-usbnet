/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FILES_H
#define FILES_H
/* The file manager's HTTP routes, under /fs/. Paths are limited to the console's
 * storage: /data, /mnt/ext0-9 and /mnt/usb0-9. Nothing here knows about the network
 * under the stream, so it runs the same on a host against a temporary directory. */
#include "http_server.h"

/* Returns HTTP_NOT_MINE for anything outside /fs/. */
int files_route(const struct http_request *request, struct http_stream *stream);
/* Where transfers are noted: one line for each, with the volume and never a file's name. */
void files_set_log(void (*log)(const char *line));
/* Whether `path` is one the routes accept. */
int files_path_allowed(const char *path);
/* Ensures a newly created file or directory is observable as 0777. Pass the
 * open descriptor for a file, or -1 to chmod/stat `path`. A chmod that the
 * filesystem does not support is accepted only when stat already shows 0777. */
int files_ensure_0777(const char *path, int fd);
/* Asks the running job to stop; waits for it. */
void files_stop(void);
/* Removes the disk test's file, left by a run that did not end. */
void files_sweep(void);
/* The one job slot, for a copy or stream install that reads from somewhere other than
 * the console (smb.c / pkg_install.c). When `to` is non-empty, files_job_run checks
 * that `to` is a free name in an existing folder; when `to` is NULL or "", there is no
 * filesystem target. Takes the slot and runs body(arg) on a thread of its own, and
 * sends the reply; *started says whether the thread took `arg`. `label` is what the
 * transfer list names. */
int files_job_run(struct http_stream *stream, const char *kind, const char *label, const char *to, const char *client, void *(*body)(void *), void *arg,
    int *started);
/* From the job's thread. files_job_size returns -1 if the target's volume lacks room. */
int files_job_size(uint64_t bytes);
void files_job_add(uint64_t bytes);
/* Monotonically sets progress (never moves backwards). */
void files_job_set(uint64_t bytes);
/* Short status/error detail exposed in /fs/job (e.g. "promoting", "0x80990005"). */
void files_job_detail(const char *detail);
int files_job_canceled(void);
/* NULL or "" for done, "canceled", or the failure's code; on failure, if `to` was set,
 * whatever the job wrote at its target is removed. Frees the slot. */
void files_job_end(const char *error_code);
#ifdef FILES_TESTING
/* One more volume, for the tests, and how often a long upload is noted. */
extern const char *files_test_root;
extern long files_test_sample_ms;
#endif
#endif
