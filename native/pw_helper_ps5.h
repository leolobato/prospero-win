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
 */
#include <stdint.h>

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
} PwHelperConfig;

typedef struct PwHelper {
    int32_t pid;                /* the child, 0 when none runs */
    int channel;                /* the socket the child keeps open while it lives, or -1 */
    int ready;                  /* its program wrote ready_path in time */
} PwHelper;

/* 0 when the child runs Wine (helper->ready says whether its program got
 * ready in time), -1 when no helper runs. */
int pw_helper_start(const PwHelperConfig *config, PwHelper *helper);
/* Whether the child still runs. */
int pw_helper_alive(const PwHelper *helper);

#endif
