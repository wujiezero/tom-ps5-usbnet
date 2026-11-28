/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "instance.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define INSTANCE_PATH_SIZE 256

static char stop_path[INSTANCE_PATH_SIZE];
static int lock_fd = -1;
static const char *failed_at;

static int format_path(char *out, size_t size, const char *directory, const char *name, pid_t pid)
{
    int count = pid ? snprintf(out, size, "%s/%s-%ld", directory, name, (long)pid)
                    : snprintf(out, size, "%s/%s", directory, name);

    if (count >= 0 && (size_t)count < size) return 0;
    errno = ENAMETOOLONG;
    return -1;
}

const char *instance_failed_at(void)
{
    return failed_at;
}

int instance_acquire(const char *directory, unsigned timeout_seconds)
{
    char path[INSTANCE_PATH_SIZE];
    struct stat info;
    const struct timespec pause = {0, 200000000};
    pid_t previous_owner = 0;
    unsigned attempt;
    int saved;

    failed_at = "control directory";
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) return -1;
    if (lstat(directory, &info) != 0) return -1;
    if (!S_ISDIR(info.st_mode) || (info.st_mode & 0077)) {
        errno = EACCES;
        return -1;
    }
    failed_at = "open lock";
    if (format_path(path, sizeof path, directory, "lock", 0) != 0 ||
            format_path(stop_path, sizeof stop_path, directory, "stop", getpid()) != 0)
        return -1;
    lock_fd = open(path, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock_fd < 0) return -1;

    for (attempt = 0; ; attempt++) {
        struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};

        failed_at = "set file lock";
        if (fcntl(lock_fd, F_SETLK, &lock) == 0) break;
        if (errno != EAGAIN && errno != EACCES) goto fail;
        if (attempt >= timeout_seconds * 5) {
            failed_at = "wait for old instance";
            errno = ETIMEDOUT;
            goto fail;
        }

        failed_at = "read lock owner";
        if (fcntl(lock_fd, F_GETLK, &lock) != 0) goto fail;
        if (lock.l_type != F_UNLCK && lock.l_pid > 0) {
            int request;

            if (!previous_owner) previous_owner = lock.l_pid;
            if (lock.l_pid == previous_owner) {
                failed_at = "request old instance to stop";
                if (format_path(path, sizeof path, directory, "stop", previous_owner) != 0) goto fail;
                request = open(path, O_CREAT | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
                if (request < 0) goto fail;
                close(request);
            }
        }
        nanosleep(&pause, NULL);
    }

    /* A previous owner with the same PID may have left a request behind. */
    failed_at = "clear stale stop request";
    if (unlink(stop_path) != 0 && errno != ENOENT) goto fail;
    return 0;

fail:
    saved = errno;
    close(lock_fd);
    lock_fd = -1;
    errno = saved;
    return -1;
}

int instance_stop_requested(void)
{
    struct stat info;

    return lock_fd >= 0 && lstat(stop_path, &info) == 0 && S_ISREG(info.st_mode);
}

void instance_release(void)
{
    if (lock_fd < 0) return;
    unlink(stop_path);
    close(lock_fd);
    lock_fd = -1;
}
