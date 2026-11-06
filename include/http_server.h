/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H
/* The page's HTTP side. It knows nothing of USB or of the stack under the stream, so a
 * browser can drive it on any host. */
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Stream handles are opaque: lwIP sockets must never be passed to OS I/O. */
struct http_stream {
    void *context;
    ssize_t (*read)(void *context, void *buffer, size_t length);
    ssize_t (*write)(void *context, const void *buffer, size_t length);
};

/* A request a backend's `route` may take. `body` is what arrived with the head. */
struct http_request {
    const char *method, *path, *query, *host, *range;
    uint64_t length;
    int has_length;
    const uint8_t *body;
    size_t body_length;
};
#define HTTP_NOT_MINE 1

struct http_backend {
    void *context;
    /* Asked first, if set. Returns HTTP_NOT_MINE, or what its response returned. */
    int (*route)(void *context, const struct http_request *request, struct http_stream *stream);
    /* Writes a JSON object for the page. */
    int (*info)(void *context, char *out, size_t capacity);
    void (*stop)(void *context);
};

/* One request per connection; the caller closes the transport. */
int http_serve(const struct http_backend *backend, struct http_stream *stream);
int http_write_all(struct http_stream *stream, const void *data, size_t length);
int http_respond(struct http_stream *stream, int status, const char *type, const char *body);
/* A percent-decoded query value. -1 if it is absent, too long or holds a NUL. */
int http_query_text(const char *query, const char *key, char *out, size_t capacity);
int http_query_flag(const char *query, const char *key);
/* 0 if it is absent or not a number. */
uint64_t http_query_number(const char *query, const char *key);
extern const char app_page[];
extern const unsigned char app_icon[];
extern const unsigned int app_icon_length;
#endif
