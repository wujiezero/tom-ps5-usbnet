/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TOM_INSTANCE_H
#define TOM_INSTANCE_H

/* The caller owns the lock until all service and device cleanup is complete. */
int instance_acquire(const char *directory, unsigned timeout_seconds);
const char *instance_failed_at(void);
int instance_stop_requested(void);
void instance_release(void);

#endif
