/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_TITLE_NOTIFY_H
#define PW_TITLE_NOTIFY_H
#include <stdint.h>

/* Code inside the game reaches the title's notifications and its invite
 * question through two private process information classes, on the
 * current process only:
 *
 * PW_TITLE_NOTIFY_PROCESS_INFO, NtSetInformationProcess with a
 * struct pw_title_notify: the title shows text as a system notification.
 *
 * PW_TITLE_INVITE_PROCESS_INFO, NtSetInformationProcess with a
 * struct pw_title_invite: the title asks the player to accept an invite
 * with L3+R3 within timeout_ms (a new question replaces an open one), or
 * forgets the open one when timeout_ms is 0. NtQueryInformationProcess
 * with the same struct fills answer: PW_TITLE_INVITE_NONE while the
 * question is open (or none is), else ACCEPTED or EXPIRED, once; the
 * question is then closed.
 *
 * Wine without the classes, or a title that does not take them, fails the
 * call (STATUS_NOT_SUPPORTED, STATUS_NOT_IMPLEMENTED or
 * STATUS_INVALID_INFO_CLASS), and the caller does without: it logs the
 * text, and answers an invite itself.
 *
 * Pointer-free wire layouts, identical for PE32, PE64 and the Unix
 * provider. Text is UTF-8, NUL-terminated. */
#define PW_TITLE_NOTIFY_PROCESS_INFO 0x50574D02u
#define PW_TITLE_INVITE_PROCESS_INFO 0x50574D03u
#define PW_TITLE_NOTIFY_VERSION 1u

enum { PW_TITLE_NOTIFY_TEXT = 1024 };
struct pw_title_notify
{
    uint32_t version;       /* PW_TITLE_NOTIFY_VERSION */
    uint32_t reserved;      /* 0 */
    char text[PW_TITLE_NOTIFY_TEXT];
};

enum { PW_TITLE_INVITE_NONE = 0, PW_TITLE_INVITE_ACCEPTED = 1, PW_TITLE_INVITE_EXPIRED = 2 };
struct pw_title_invite
{
    uint32_t version;       /* PW_TITLE_NOTIFY_VERSION */
    uint32_t timeout_ms;    /* set: how long the player has; 0 forgets the question */
    uint32_t answer;        /* query: PW_TITLE_INVITE_* */
    uint32_t reserved;      /* 0 */
};

#endif
