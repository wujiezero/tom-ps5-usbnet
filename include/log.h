/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOG_H
#define LOG_H
#include <stddef.h>
#include <stdint.h>

/* The run log: APP_DIR/log.txt, with the previous run's in log.old.txt. Lines also go
 * to the ELF loader's socket. Each starts with the seconds since the log was opened. Never a serial number, a MAC address, a LAN address or a
 * file's name: console_line() is for what only the person at the loader should see. */
#define APP_NAME "tom-ps5-usbnet"
#define APP_VERSION "0.3.1"
#define APP_DIR "/data/" APP_NAME

int log_open(void);
void log_close(void);
void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void console_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void checkpoint(void);                  /* puts what was said on disk */
void notify(const char *text);          /* a notification on the console's screen */
void log_firmware(void);
#endif
