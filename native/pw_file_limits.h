/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_FILE_LIMITS_H
#define PW_FILE_LIMITS_H
/* The title's descriptor limit, raised to its hard limit before Wine starts,
 * with both values kept for the log. Wine's __wine_main raises it the same
 * way; doing it here first records what the title was given.
 *
 * It is not what stops games that keep many files open: a PS5 title can hold
 * about 256 files from open() whatever RLIMIT_NOFILE says (13952 on FW 13.60),
 * and the server keeps that count down by parking read-only files
 * (wine/patches/0993). */
#include <sys/resource.h>

typedef struct PwFileLimitsOps {
    int (*get)(int resource, struct rlimit *limit);
    int (*set)(int resource, const struct rlimit *limit);
} PwFileLimitsOps;

typedef struct PwFileLimits {
    long long soft_before, hard_before;  /* -1 when unknown */
    long long soft_after, hard_after;    /* read again after the raise */
    int get_errno;  /* a read failed: the values not read stay -1 */
    int set_errno;  /* the raise failed (0 when not needed) */
} PwFileLimits;

/* 0 when the soft limit equals the hard one afterwards, -1 otherwise. */
int pw_file_limits_raise_with(const PwFileLimitsOps *ops, PwFileLimits *limits);
int pw_file_limits_raise(PwFileLimits *limits);

#endif
