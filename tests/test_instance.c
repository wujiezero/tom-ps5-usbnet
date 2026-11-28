/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "instance.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t start_owner(const char *path, int respond)
{
    int ready[2];
    char signal_byte;
    pid_t child;

    assert(pipe(ready) == 0);
    child = fork();
    assert(child >= 0);
    if (!child) {
        assert(close(ready[0]) == 0);
        assert(instance_acquire(path, 0) == 0);
        assert(write(ready[1], "1", 1) == 1);
        while (!respond || !instance_stop_requested()) usleep(10000);
        usleep(300000);
        instance_release();
        _exit(0);
    }
    assert(close(ready[1]) == 0);
    assert(read(ready[0], &signal_byte, 1) == 1);
    assert(close(ready[0]) == 0);
    return child;
}

int main(void)
{
    char directory[] = "/tmp/tom-instance.XXXXXX", lock[128];
    int status;
    pid_t child;

    alarm(10);
    assert(mkdtemp(directory));
    child = start_owner(directory, 1);
    assert(instance_acquire(directory, 3) == 0);
    assert(!instance_stop_requested());
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    instance_release();

    child = start_owner(directory, 0);
    assert(instance_acquire(directory, 1) == -1 && errno == ETIMEDOUT);
    assert(kill(child, SIGKILL) == 0);
    assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
    assert(instance_acquire(directory, 0) == 0);
    instance_release();

    assert(snprintf(lock, sizeof lock, "%s/lock", directory) > 0);
    assert(unlink(lock) == 0);
    assert(snprintf(lock, sizeof lock, "%s/stop-%ld", directory, (long)child) > 0);
    assert(unlink(lock) == 0 && rmdir(directory) == 0);
    puts("test-instance: handoff, timeout and reacquire after owner dies OK");
    return 0;
}
