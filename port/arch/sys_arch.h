/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef UN_ARCH_SYS_ARCH_H
#define UN_ARCH_SYS_ARCH_H
#include <pthread.h>
struct un_sys_sem;
struct un_sys_mbox;
typedef struct un_sys_sem *sys_sem_t;
typedef struct un_sys_mbox *sys_mbox_t;
typedef pthread_mutex_t *sys_mutex_t;
typedef pthread_t sys_thread_t;
typedef unsigned int sys_prot_t;
#define SYS_SEM_NULL NULL
#define SYS_MBOX_NULL NULL
/* Suppress lwIP's optional generic implementation in core/sys.c. */
#define sys_msleep un_sys_msleep
void un_sys_msleep(u32_t milliseconds);
#define sys_sem_valid(s) ((s) != NULL && *(s) != NULL)
#define sys_sem_set_invalid(s) (*(s) = NULL)
#define sys_mbox_valid(m) ((m) != NULL && *(m) != NULL)
#define sys_mbox_set_invalid(m) (*(m) = NULL)
#define sys_mutex_valid(m) ((m) != NULL && *(m) != NULL)
#define sys_mutex_set_invalid(m) (*(m) = NULL)
#endif
