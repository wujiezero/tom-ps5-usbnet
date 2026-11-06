/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Forced into the sources that talk to the adapter, so that a real thread-safe
 * simulation of ugen stands behind them. See test_app.c. */
#ifndef MOCK_ADAPTER_H
#define MOCK_ADAPTER_H
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef __packed
#define __packed __attribute__((__packed__))
#endif

int mock_ioctl(int fd, unsigned long request, void *arg);
int mock_open(const char *path, int flags, ...);
int mock_close(int fd);
int mock_stat(const char *path, struct stat *st);
int mock_poll(struct pollfd *fds, nfds_t n, int timeout);
int mock_unlink(const char *path);

#define ioctl(fd, request, arg) mock_ioctl(fd, request, arg)
#define open(...) mock_open(__VA_ARGS__)
#define close(fd) mock_close(fd)
#define stat(path, st) mock_stat(path, st)
#define poll(fds, n, timeout) mock_poll(fds, n, timeout)
#define unlink(path) mock_unlink(path)
#endif
