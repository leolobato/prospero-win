/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_HELPER_PS5_H
#define PW_HELPER_PS5_H
/*
 * The title's side of [helper]: start a local process beside the game, give
 * it the /data grant and a connection to the title's Wine server, and wait
 * until its program says it is ready. The child is native/pw_helper_child.c.
 *
 * Called from Wine's main thread through the server-connected hook (Wine
 * patch 1013), so the game does not run before this returns. Every failure
 * is logged and returned: the game then starts without its helper.
 *
 * With notify, the player sees why a helper did not get ready (it did not
 * start, ran out of time, stopped, or its ready file says "failed <reason>"),
 * named by its program (src/pw_helper_notify.h). When a file named like the
 * ready file plus ".notify" is there as the helper starts, the title empties
 * it, shows its own steps, and shows each line the program adds to it while
 * the title waits: a step-by-step trace for debugging a helper's start.
 *
 * With message, the program also reaches the player while the game runs:
 * its messages and an L3+R3 question, through the files that
 * src/pw_helper_channel.h describes. pw_helper_start makes them; the title's
 * loop then calls pw_helper_session_poll and takes the question's answer.
 */
#include <stddef.h>
#include <stdint.h>

#include "../src/pw_helper_channel.h"

typedef struct PwHelperConfig {
    const char *title_id;
    const char *child_path;     /* the title's bdj.elf, as the system service resolves it: "/app0/bdj.elf" */
    const char *grant_helper;   /* the title's lapy.elf in the granted tree; NULL when the title has no grant */
    const char *runtime_dir;    /* where ntdll.prx is, in the granted tree */
    const char *prefix;         /* the bottle */
    const char *program;        /* the Windows program */
    const char *winedebug;      /* WINEDEBUG for the child's Wine, or NULL */
    const char *log_path;       /* the child's Wine messages, or NULL */
    const char *ready_path;     /* a file the program writes when the game may start, or NULL */
    uint32_t wait_seconds;      /* how long to wait for ready_path */
    /* One more client socket from the title's Wine server (pw_wineserver_connect). */
    int (*server_connect)(void *context);
    void *context;
    void (*log)(const char *line);
    /* Shows text as a system notification; NULL shows nothing. */
    int (*notify)(const char *text);
    /* Shows a message of the program while the game runs, as a system
     * notification; NULL gives the program no files to reach the player. The
     * text is the program's: it goes to the screen, not the log. */
    int (*message)(const char *text);
} PwHelperConfig;

/* The program's messages and question while the game runs. */
typedef struct PwHelperSession {
    char say[512], ask[512], answer[512];   /* "" when the title has no such file */
    size_t said;                /* bytes of say already shown */
    uint64_t next_look_ms;
    uint64_t last_id;           /* the last question taken from ask */
    uint64_t open_id;           /* the question the player can answer now, 0 for none */
    uint64_t deadline_ms;
    int can_ask;                /* answer is there: the program may ask */
    unsigned dropped;           /* messages over the limit, not yet logged */
    PwHelperSayLimit limit;
} PwHelperSession;

typedef struct PwHelper {
    int32_t pid;                /* the child, 0 when none runs */
    int channel;                /* the socket the child keeps open while it lives, or -1 */
    int ready;                  /* its program wrote ready_path in time */
    PwHelperSession session;
} PwHelper;

/* 0 when the child runs Wine (helper->ready says whether its program got
 * ready in time), -1 when no helper runs. */
int pw_helper_start(const PwHelperConfig *config, PwHelper *helper);
/* Whether the child still runs. */
int pw_helper_alive(const PwHelper *helper);

/* From the title's loop while the game runs, after pw_helper_start has
 * returned: shows the program's new messages, takes a new question and
 * expires an open one. It looks at the files every 250 ms whatever the call
 * rate. can_ask: the title can take L3+R3 from the pad now; the answer file
 * is made the first time it can. now_ms is a steady clock. */
void pw_helper_session_poll(PwHelper *helper, const PwHelperConfig *config, uint64_t now_ms, int can_ask);
/* The open question's id, 0 when none is open. */
uint64_t pw_helper_session_question(const PwHelper *helper);
/* The player accepted the open question: the program gets "<id> accepted". */
void pw_helper_session_accept(PwHelper *helper, const PwHelperConfig *config);

#endif
