/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See log.h. */
#include "log.h"

#include <sys/stat.h>

#include <ps5/kernel.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOG_FILE APP_DIR "/log.txt"
#define OLD_LOG_FILE APP_DIR "/log.old.txt"

typedef struct {
    char reserved[45];
    char message[3075];
} notify_request;
typedef int (*notify_fn)(int, notify_request *, size_t, int);

static int g_log = -1;

static void write_all(int fd, const char *data, size_t left)
{
    while (left) {
        ssize_t n = write(fd, data, left);

        if (n <= 0) return;
        data += n;
        left -= (size_t)n;
    }
}

static size_t format_line(char *line, size_t capacity, const char *fmt, va_list ap)
{
    int n = vsnprintf(line, capacity - 1, fmt, ap);

    if (n < 0) return 0;
    if (n > (int)capacity - 2) n = (int)capacity - 2;
    line[n++] = '\n';
    return (size_t)n;
}

void say(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    size_t n;

    va_start(ap, fmt);
    n = format_line(line, sizeof line, fmt, ap);
    va_end(ap);
    write_all(1, line, n);
    if (g_log >= 0) write_all(g_log, line, n);
}

void console_line(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    size_t n;

    va_start(ap, fmt);
    n = format_line(line, sizeof line, fmt, ap);
    va_end(ap);
    write_all(1, line, n);
}

void checkpoint(void)
{
    if (g_log >= 0) fsync(g_log);
}

/* Resolved at run time: a missing symbol at link level stops a payload before main(),
 * and a notification is not worth that. */
void notify(const char *text)
{
    static notify_fn request_notification;
    static int resolved;
    notify_request request;

    if (!resolved) {
        resolved = 1;
        request_notification = (notify_fn)dlsym(RTLD_DEFAULT, "sceKernelSendNotificationRequest");
    }
    if (!request_notification) return;
    memset(&request, 0, sizeof request);
    snprintf(request.message, sizeof request.message, APP_NAME ": %s", text);
    request_notification(0, &request, sizeof request, 0);
}

void log_firmware(void)
{
    uint32_t firmware = kernel_get_fw_version();

    say("%s %s, firmware %x.%02x", APP_NAME, APP_VERSION, (unsigned)(firmware >> 24) & 0xff, (unsigned)(firmware >> 16) & 0xff);
    checkpoint();
}

int log_open(void)
{
    mkdir(APP_DIR, 0777);
    rename(LOG_FILE, OLD_LOG_FILE);
    g_log = open(LOG_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    return g_log >= 0 ? 0 : -1;
}

void log_close(void)
{
    if (g_log >= 0) close(g_log);
    g_log = -1;
}
