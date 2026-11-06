/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A fatal signal ends the log with the instruction, the address and the last step.
 * SIGKILL and a console reset leave nothing, which tells those two apart.
 *
 * The handler calls what a signal handler should not, because the process is going
 * anyway. SA_RESETHAND means a fault in here ends it at once. */
#include "crash.h"
#include "log.h"

#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#ifdef CRASH_TESTING
#define FAULT_INSTRUCTION(state) ((void)(state), 0UL)   /* the field's name differs off FreeBSD */
#else
#define FAULT_INSTRUCTION(state) ((unsigned long)(state)->uc_mcontext.mc_rip)
#endif

static const char *volatile g_stage = "start";

void crash_stage(const char *what)
{
    g_stage = what;
}

static void fatal(int signo, siginfo_t *info, void *context)
{
    const ucontext_t *state = context;

    say("fatal signal %d: instruction %#lx, address %p, this ELF's handler at %p, last step: %s", signo,
        FAULT_INSTRUCTION(state), info->si_addr, (void *)fatal, g_stage);
    checkpoint();
    _exit(3);
}

static void catch_signal(int signo)
{
    struct sigaction action;

    memset(&action, 0, sizeof action);
    action.sa_sigaction = fatal;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    sigaction(signo, &action, NULL);
}

void crash_log_start(void)
{
    static const int fatal_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS, SIGTERM, SIGHUP};
    size_t i;

    for (i = 0; i < sizeof fatal_signals / sizeof fatal_signals[0]; i++) catch_signal(fatal_signals[i]);
    /* A browser that goes away must end a write, not the process. */
    signal(SIGPIPE, SIG_IGN);
}
