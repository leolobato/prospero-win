/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_HELPER_MAILBOX_H
#define PW_HELPER_MAILBOX_H
/*
 * What a title and the local process it starts ([helper] in the profile) tell
 * each other before they share a socket.
 *
 * The title has the /data grant by then: it sees the console's real file
 * tree. The child starts in the title's sandbox and sees another one, so no
 * path names the same file for both. A named shared memory object does: its
 * name is not a path in either tree. The title creates it, the child opens
 * it, and each side polls the other's word. Once the child has the grant too
 * (the title asks for it, the child clones its credentials on request), both
 * see the same tree and the title passes Wine's server socket over a Unix
 * socket whose path is in the mailbox.
 */
#include <stddef.h>
#include <stdint.h>

#define PW_HELPER_MAILBOX_MAGIC UINT32_C(0x31504c48) /* HLP1 */
enum { PW_HELPER_PATH = 512, PW_HELPER_SOCKET_PATH = 96, PW_HELPER_LOG = 2048, PW_HELPER_NAME = 48 };

/* The title's word. */
enum pw_helper_step {
    PW_HELPER_STEP_WAIT = 0,
    PW_HELPER_STEP_CLONE,     /* call seteuid(geteuid()) and answer */
    PW_HELPER_STEP_CONNECT,   /* the grant is in place: connect to socket_path and start Wine */
    PW_HELPER_STEP_STOP       /* leave: the title could not finish the start */
};
/* The child's word. */
enum pw_helper_answer {
    PW_HELPER_ANSWER_NONE = 0,
    PW_HELPER_ANSWER_HELLO,   /* child_pid is set */
    PW_HELPER_ANSWER_CLONED,
    PW_HELPER_ANSWER_CLONE_FAILED,
    PW_HELPER_ANSWER_STARTED, /* Wine's main thread runs */
    PW_HELPER_ANSWER_FAILED   /* child_error and log say why; the child has left */
};

struct pw_helper_mailbox {
    uint32_t magic;
    uint32_t size;                       /* sizeof(struct pw_helper_mailbox) */
    volatile uint32_t step;
    volatile uint32_t answer;
    volatile int32_t child_pid;
    volatile int32_t child_error;
    char socket_path[PW_HELPER_SOCKET_PATH];
    char runtime_dir[PW_HELPER_PATH];    /* where ntdll.prx is, in the granted tree */
    char prefix[PW_HELPER_PATH];         /* the bottle */
    char program[PW_HELPER_PATH];        /* the Windows program the child's Wine runs */
    char winedebug[PW_HELPER_PATH];
    char log_path[PW_HELPER_PATH];       /* Wine's messages of the child; empty for none */
    volatile uint32_t log_used;          /* the child's own lines until it can write log_path */
    char log[PW_HELPER_LOG];
};

/* "/pw-helper-<title id>": one title runs at a time. 0, or -1 when it does not fit. */
static inline int pw_helper_mailbox_name(char *name, size_t size, const char *title_id)
{
    static const char prefix[] = "/pw-helper-";
    size_t used = sizeof(prefix) - 1, i;

    if (!name || !title_id || !title_id[0] || size <= used) return -1;
    for (i = 0; i < used; i++) name[i] = prefix[i];
    for (i = 0; title_id[i]; i++) {
        const char c = title_id[i];
        if (used + 1 >= size) return -1;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return -1;
        name[used++] = c;
    }
    name[used] = '\0';
    return 0;
}

#endif
