/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_INVITE_PROMPT_H
#define PW_INVITE_PROMPT_H
#include <stdint.h>

/* The answer to an invite question code in the game asked the title
 * (wine/ps5/menu/pw_title_notify.h): while it is open, L3+R3 pressed
 * together accepts it. That press is the question's: the game does not see
 * it, and it does not open the in-game menu (src/pw_game_menu.h), which
 * reads the pad after this. L3 and R3 stay hidden from the game until they
 * are let go. Everything else passes through. */
typedef struct PwInvitePrompt {
    uint32_t withheld;      /* buttons the game must not see yet */
    uint32_t game_held;     /* what the game saw held last tick */
} PwInvitePrompt;

/* The pad as the game (and the menu) should see it this tick. */
typedef struct PwInvitePromptView {
    uint32_t held, pressed, released;
    int accept;             /* 1: the player accepted, this tick only */
} PwInvitePromptView;

/* DualSense buttons as scePadRead reports them (the menu's). */
enum { PW_INVITE_PROMPT_L3 = 0x2u, PW_INVITE_PROMPT_R3 = 0x4u,
       PW_INVITE_PROMPT_COMBO = PW_INVITE_PROMPT_L3 | PW_INVITE_PROMPT_R3 };

/* One tick of the title's loop with the pad's held buttons, the edges since
 * the last tick, and whether a question is open (pending; the title passes
 * 0 while its in-game menu is open, so L3+R3 closes the menu). Fills *view
 * and returns view->accept. */
int pw_invite_prompt_tick(PwInvitePrompt *prompt, uint32_t held, uint32_t pressed, uint32_t released,
                          int pending, PwInvitePromptView *view);

#endif
