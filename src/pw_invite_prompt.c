/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_invite_prompt.h"

int pw_invite_prompt_tick(PwInvitePrompt *prompt, uint32_t held, uint32_t pressed, uint32_t released,
                          int pending, PwInvitePromptView *view)
{
    int combo = (held & PW_INVITE_PROMPT_COMBO) == PW_INVITE_PROMPT_COMBO && (pressed & PW_INVITE_PROMPT_COMBO);
    uint32_t game;

    if (!prompt || !view) return 0;
    view->accept = pending && combo;
    prompt->withheld &= held;
    if (view->accept) prompt->withheld |= PW_INVITE_PROMPT_COMBO;
    game = held & ~prompt->withheld;
    view->held = game;
    view->pressed = (pressed & ~prompt->withheld) | (game & ~prompt->game_held);
    /* As the menu does: a release is the game's only for a button it saw
     * down, or one pressed and let go within this tick that it is shown
     * pressed. */
    view->released = (prompt->game_held & ~game) | (released & pressed & ~prompt->withheld & ~game);
    prompt->game_held = game;
    return view->accept;
}
