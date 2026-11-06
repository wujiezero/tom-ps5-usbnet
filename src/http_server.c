/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See http_server.h. Serves one fixed page and the backend's routes, and touches no file
 * itself. A POST from another site's page is refused by its Origin header. */
#include "http_server.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_HEAD 16384
#define MAX_FIELD 160
#define MAX_QUERY 8192                  /* two percent-encoded paths */
#define JSON_BYTES 1024

struct request {
    char method[8], path[32], query[MAX_QUERY], host[MAX_FIELD], origin[MAX_FIELD], range[64];
    uint64_t length;
    int has_length;
    size_t head_used, body_prefix;
};

int http_write_all(struct http_stream *stream, const void *data, size_t length)
{
    const uint8_t *p = data;

    while (length) {
        ssize_t n = stream->write(stream->context, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        length -= (size_t)n;
    }
    return 0;
}

static int respond(struct http_stream *stream, int status, const char *type, const char *body)
{
    char head[256];
    const char *text = status == 200 ? "OK" : status == 206 ? "Partial Content" : status == 400 ? "Bad Request" : status == 403 ? "Forbidden" :
        status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : status == 409 ? "Conflict" :
        status == 413 ? "Payload Too Large" : "Service Unavailable";
    size_t size = strlen(body);
    int n = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, text, type, size);

    if (http_write_all(stream, head, (size_t)n) != 0) return -1;
    return http_write_all(stream, body, size);
}

int http_respond(struct http_stream *stream, int status, const char *type, const char *body)
{
    return respond(stream, status, type, body);
}

static int respond_json(struct http_stream *stream, int result, const char *json)
{
    return respond(stream, result == 0 ? 200 : 503, "application/json", json[0] ? json : "{}");
}

static void copy_field(char *out, size_t capacity, const char *value, size_t length)
{
    if (length >= capacity) length = capacity - 1;
    memcpy(out, value, length);
    out[length] = 0;
}

static const char *line_end(const char *line, const char *end)
{
    const char *p;

    for (p = line; p + 1 < end; p++)
        if (p[0] == '\r' && p[1] == '\n') return p;
    return NULL;
}

static int parse_header(struct request *q, const char *line, size_t length)
{
    const char *colon = memchr(line, ':', length);
    const char *value;
    size_t name_length, value_length;

    if (!colon) return -1;
    name_length = (size_t)(colon - line);
    value = colon + 1;
    value_length = length - name_length - 1;
    while (value_length && (*value == ' ' || *value == '\t')) {
        value++;
        value_length--;
    }
    if (name_length == 4 && !strncasecmp(line, "host", 4)) {
        copy_field(q->host, sizeof q->host, value, value_length);
    } else if (name_length == 6 && !strncasecmp(line, "origin", 6)) {
        copy_field(q->origin, sizeof q->origin, value, value_length);
    } else if (name_length == 5 && !strncasecmp(line, "range", 5)) {
        copy_field(q->range, sizeof q->range, value, value_length);
    } else if (name_length == 14 && !strncasecmp(line, "content-length", 14)) {
        uint64_t total = 0;
        size_t i;

        if (!value_length || value_length > 19) return -1;
        for (i = 0; i < value_length; i++) {
            if (value[i] < '0' || value[i] > '9') return -1;
            total = total * 10 + (uint64_t)(value[i] - '0');
        }
        q->length = total;
        q->has_length = 1;
    }
    return 0;
}

static int parse_request_line(struct request *q, const char *line, size_t length)
{
    const char *space = memchr(line, ' ', length);
    const char *target, *second, *mark;

    if (!space || (size_t)(space - line) >= sizeof q->method) return -1;
    copy_field(q->method, sizeof q->method, line, (size_t)(space - line));
    target = space + 1;
    second = memchr(target, ' ', length - (size_t)(target - line));
    if (!second || *target != '/') return -1;
    mark = memchr(target, '?', (size_t)(second - target));
    if (!mark) mark = second;
    if ((size_t)(mark - target) >= sizeof q->path) return -1;
    copy_field(q->path, sizeof q->path, target, (size_t)(mark - target));
    if (mark != second) {
        if ((size_t)(second - mark - 1) >= sizeof q->query) return -1;  /* a cut path would be another path */
        copy_field(q->query, sizeof q->query, mark + 1, (size_t)(second - mark - 1));
    }
    return 0;
}

/* Reads up to the blank line. Whatever arrived after it stays at the end of `head`. */
static int read_head(struct http_stream *stream, char *head, struct request *q)
{
    const char *line, *end, *found = NULL;

    memset(q, 0, sizeof *q);
    while (!found) {
        ssize_t n;

        if (q->head_used == MAX_HEAD) return -1;
        n = stream->read(stream->context, head + q->head_used, MAX_HEAD - q->head_used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        q->head_used += (size_t)n;
        for (line = head; line + 3 < head + q->head_used; line++)
            if (!memcmp(line, "\r\n\r\n", 4)) {
                found = line;
                break;
            }
    }
    end = found + 2;
    q->body_prefix = q->head_used - (size_t)(found + 4 - head);
    line = head;
    {
        const char *stop = line_end(line, end);

        if (!stop || parse_request_line(q, line, (size_t)(stop - line)) != 0) return -1;
        line = stop + 2;
    }
    while (line < end) {
        const char *stop = line_end(line, end);

        if (!stop || parse_header(q, line, (size_t)(stop - line)) != 0) return -1;
        line = stop + 2;
    }
    return 0;
}

static const char *query_value(const char *query, const char *key)
{
    size_t key_length = strlen(key);
    const char *p = query;

    while (*p) {
        const char *next = strchr(p, '&');

        if (!strncmp(p, key, key_length) && p[key_length] == '=') return p + key_length + 1;
        if (!next) break;
        p = next + 1;
    }
    return NULL;
}

static int hex_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int http_query_text(const char *query, const char *key, char *out, size_t capacity)
{
    const char *p = query_value(query, key);
    size_t used = 0;

    if (!p) return -1;
    while (*p && *p != '&') {
        int c = (unsigned char)*p++;

        if (c == '%') {
            int high = hex_digit(p[0]), low = high < 0 ? -1 : hex_digit(p[1]);

            if (low < 0) return -1;
            c = high * 16 + low;
            p += 2;
        }
        if (!c || used + 1 >= capacity) return -1;
        out[used++] = (char)c;
    }
    out[used] = 0;
    return 0;
}

uint64_t http_query_number(const char *query, const char *key)
{
    const char *p = query_value(query, key);

    return p && *p >= '0' && *p <= '9' ? strtoull(p, NULL, 10) : 0;
}

int http_query_flag(const char *query, const char *key)
{
    const char *p = query_value(query, key);

    return p && *p == '1';
}

/* A name in Host could be another site's, rebound to this address. */
static int host_is_address(const char *host)
{
    if (!strncmp(host, "localhost", 9) && (!host[9] || host[9] == ':')) return 1;
    for (; *host; host++)
        if (!((*host >= '0' && *host <= '9') || *host == '.' || *host == ':')) return 0;
    return 1;
}

static int same_origin(const struct request *q)
{
    char expected[MAX_FIELD + 8];

    if (!q->origin[0]) return 1;
    snprintf(expected, sizeof expected, "http://%s", q->host);
    return !strcmp(expected, q->origin);
}

int http_serve(const struct http_backend *b, struct http_stream *stream)
{
    char head[MAX_HEAD], json[JSON_BYTES];
    struct request q;
    int post;

    if (read_head(stream, head, &q) != 0) return respond(stream, 400, "text/plain", "bad request");
    post = !strcmp(q.method, "POST");
    json[0] = 0;
    if (!post && strcmp(q.method, "GET")) return respond(stream, 405, "text/plain", "method not allowed");
    if (post && !same_origin(&q)) return respond(stream, 403, "text/plain", "other origin");
    if (!host_is_address(q.host)) return respond(stream, 403, "text/plain", "other host");
    if (b->route) {
        struct http_request request = {q.method, q.path, q.query, q.host, q.range, q.length, q.has_length,
            (const uint8_t *)head + q.head_used - q.body_prefix, q.body_prefix};
        int handled = b->route(b->context, &request, stream);

        if (handled != HTTP_NOT_MINE) return handled;
    }
    if (!post) {
        if (!strcmp(q.path, "/")) return respond(stream, 200, "text/html; charset=utf-8", app_page);
        if (!strcmp(q.path, "/api/info")) return respond_json(stream, b->info(b->context, json, sizeof json), json);
    } else if (!strcmp(q.path, "/api/exit")) {
        int result = respond_json(stream, 0, json);

        b->stop(b->context);
        return result;
    }
    return respond(stream, 404, "text/plain", "not found");
}
