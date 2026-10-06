/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_TITLE_PUBLICATION_PS5_LOCK_H
#define PW_TITLE_PUBLICATION_PS5_LOCK_H

/* Checked libkernel syscall gateway; fail closed if unavailable. */
int pw_title_publication_ps5_lock(void *context, int descriptor);
/* Sets the process file-creation mask through the same libkernel gateway;
 * returns the previous mask, or -1 with errno. */
int pw_title_ps5_umask(unsigned mask);

#endif
