/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A fault ends the process with a line in the log, and exit status 3. */
#include "crash.h"
#include "log.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_pipe[2];

void say(const char *format, ...)
{
    char line[256];
    va_list ap;
    int n;

    va_start(ap, format);
    n = vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    assert(write(g_pipe[1], line, (size_t)n) == n);
}

void checkpoint(void) {}

int main(void)
{
    char said[256] = "";
    int status;
    pid_t child;

    assert(pipe(g_pipe) == 0);
    child = fork();
    assert(child >= 0);
    if (!child) {
        crash_log_start();
        crash_stage("the test");
        *(volatile int *)8 = 1;
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 3);
    assert(read(g_pipe[0], said, sizeof said - 1) > 0);
    assert(strstr(said, "fatal signal") && strstr(said, "last step: the test"));
    printf("test-crash: all OK\n");
    return 0;
}
