/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "forward_http.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static int equal(const char *p, size_t n, const char *expected)
{
    return strlen(expected) == n && !strncasecmp(p, expected, n);
}

static int token(unsigned char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || (ch && strchr("!#$%&'*+-.^_`|~", ch));
}

static int append(char **out, size_t *left, const char *p, size_t n)
{
    if (*left < n) return -1;
    memcpy(*out, p, n);
    *out += n;
    *left -= n;
    return 0;
}

static int decimal(const char *p, const char *end, uint64_t *value)
{
    if (p == end) return -1;
    *value = 0;
    for (; p < end; p++) {
        unsigned digit = (unsigned char)*p - '0';

        if (digit > 9 || *value > (UINT64_MAX - digit) / 10) return -1;
        *value = *value * 10 + digit;
    }
    return 0;
}

int forward_http_head(const char *head, size_t length, const char *external,
                      const char *upstream, char *output, size_t capacity, uint64_t *body)
{
    const char *line = head, *end = head + length;
    char local_origin[64], mapped_host[64], mapped_origin[72], *out = output;
    int host = 0, origin = 0, content = 0, request_line = 1;
    size_t left = capacity;

    snprintf(local_origin, sizeof local_origin, "http://%s", external);
    snprintf(mapped_host, sizeof mapped_host, "Host: %s\r\n", upstream);
    snprintf(mapped_origin, sizeof mapped_origin, "Origin: http://%s\r\n", upstream);
    *body = 0;
    while (line < end) {
        const char *stop = memchr(line, '\r', (size_t)(end - line));
        const char *colon, *p, *value, *last;
        size_t name, n;

        if (!stop || end - stop < 2 || stop[1] != '\n') return -1;
        if (stop == line) {
            static const char closing[] = "Connection: close\r\n\r\n";

            if (!host || stop + 2 != end || append(&out, &left, closing, sizeof closing - 1) != 0) return -1;
            return (int)(out - output);
        }
        if (request_line) {
            const char *space = memchr(line, ' ', (size_t)(stop - line));

            if (!space || space == line || stop - space < 11 || space[1] != '/' ||
                    memcmp(stop - 9, " HTTP/1.1", 9)) return -1;
            for (p = line; p < space; p++)
                if (!token((unsigned char)*p)) return -1;
            for (p = space + 1; p < stop - 9; p++)
                if ((unsigned char)*p <= ' ' || *p == 127) return -1;
            if (append(&out, &left, line, (size_t)(stop + 2 - line)) != 0) return -1;
            request_line = 0;
            line = stop + 2;
            continue;
        }
        colon = memchr(line, ':', (size_t)(stop - line));
        if (!colon || colon == line) return -1;
        for (p = line; p < colon; p++)
            if (!token((unsigned char)*p)) return -1;
        for (p = colon + 1; p < stop; p++)
            if (((unsigned char)*p < ' ' && *p != '\t') || *p == 127) return -1;
        value = colon + 1;
        while (value < stop && (*value == ' ' || *value == '\t')) value++;
        last = stop;
        while (last > value && (last[-1] == ' ' || last[-1] == '\t')) last--;
        name = (size_t)(colon - line);
        n = (size_t)(last - value);

        if (equal(line, name, "Host")) {
            if (host++ || !equal(value, n, external) ||
                    append(&out, &left, mapped_host, strlen(mapped_host)) != 0) return -1;
        } else if (equal(line, name, "Origin")) {
            if (origin++ || !equal(value, n, local_origin) ||
                    append(&out, &left, mapped_origin, strlen(mapped_origin)) != 0) return -1;
        } else if (equal(line, name, "Connection")) {
            if (!equal(value, n, "close") && !equal(value, n, "keep-alive")) return -1;
        } else if (equal(line, name, "Transfer-Encoding") || equal(line, name, "Upgrade") ||
                   equal(line, name, "Trailer") || equal(line, name, "TE") ||
                   equal(line, name, "Proxy-Connection") || equal(line, name, "Proxy-Authorization")) {
            return -1;
        } else {
            if (equal(line, name, "Content-Length") &&
                    (content++ || decimal(value, last, body) != 0)) return -1;
            if (append(&out, &left, line, (size_t)(stop + 2 - line)) != 0) return -1;
        }
        line = stop + 2;
    }
    return -1;
}
