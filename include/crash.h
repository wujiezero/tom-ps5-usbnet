/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CRASH_H
#define CRASH_H

/* Writes a fatal signal to the log before the process goes. See crash.c. */
void crash_log_start(void);
/* What the log should name as the last step. The text must stay valid for good. */
void crash_stage(const char *what);
#endif
