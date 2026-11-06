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
/* Asks the running job to stop; waits for it. */
void files_stop(void);
/* Removes the disk test's file, left by a run that did not end. */
void files_sweep(void);
#ifdef FILES_TESTING
/* One more volume, for the tests. */
extern const char *files_test_root;
#endif
#endif
