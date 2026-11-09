/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_invite_prompt.h"
#include "../src/pw_game_menu.h"
#include <assert.h>
#include <string.h>

enum { L3 = PW_INVITE_PROMPT_L3, R3 = PW_INVITE_PROMPT_R3, SQUARE = 0x8000u, CIRCLE = PW_GAME_MENU_CIRCLE };

static PwInvitePrompt prompt;
static PwInvitePromptView view;
static PwGameMenu menu;
static PwGameMenuView menu_view;
static struct pw_title_menu shown;
static uint32_t held;

/* One tick of the title's chain, the prompt before the menu, from the
 * previous held set to `now`; edges derived as the pad core does. Returns
 * whether the prompt accepted. */
static int tick(uint32_t now, int pending)
{
    uint32_t pressed = now & ~held, released = held & ~now;
    held = now;
    int accept = pw_invite_prompt_tick(&prompt, now, pressed, released, pending && !menu.open, &view);
    pw_game_menu_tick(&menu, view.held, view.pressed, view.released, &shown, &menu_view);
    return accept;
}

static void assert_game(uint32_t h, uint32_t p, uint32_t r)
{
    assert(menu_view.held == h && menu_view.pressed == p && menu_view.released == r);
}

int main(void)
{
    /* No question: the pad is the game's and L3+R3 opens the menu. */
    assert(!tick(SQUARE, 0));
    assert_game(SQUARE, SQUARE, 0);
    assert(view.held == SQUARE && view.pressed == SQUARE && !view.accept);
    assert(!tick(SQUARE | L3 | R3, 0) && menu.open);
    assert(!tick(0, 0) && !tick(L3 | R3, 0) && !menu.open);   /* and closes it */
    assert(!tick(0, 0));
    assert_game(0, 0, 0);

    /* A question open: L3+R3 accepts it once, the game never sees it and
     * the menu stays closed. */
    assert(!tick(SQUARE, 1));
    assert_game(SQUARE, SQUARE, 0);
    assert(!tick(SQUARE | L3, 1));
    assert_game(SQUARE | L3, L3, 0);         /* L3 alone is the game's */
    assert(tick(SQUARE | L3 | R3, 1));
    assert(!menu.open && !shown.open);
    assert_game(SQUARE, 0, L3);              /* L3 taken back, R3 never shown */
    assert(!tick(SQUARE | L3 | R3, 1));      /* still held: no second answer */
    assert_game(SQUARE, 0, 0);
    assert(!tick(SQUARE | R3, 1));           /* L3 let go: still not the game's */
    assert_game(SQUARE, 0, 0);
    assert(!tick(SQUARE, 0));
    assert_game(SQUARE, 0, 0);
    assert(!prompt.withheld);
    assert(!tick(SQUARE | L3, 0));           /* a new press is the game's again */
    assert_game(SQUARE | L3, L3, 0);
    assert(!tick(0, 0));
    assert_game(0, 0, SQUARE | L3);

    /* Both pressed in one tick. */
    assert(tick(L3 | R3, 1));
    assert_game(0, 0, 0);
    assert(!menu.open);
    assert(!tick(0, 1));
    assert_game(0, 0, 0);

    /* With the menu open the question waits: L3+R3 closes the menu. */
    assert(!tick(L3 | R3, 0) && menu.open);
    assert(!tick(0, 1));
    assert(!tick(L3 | R3, 1) && !menu.open);
    assert(!tick(0, 1));
    assert(tick(L3 | R3, 1) && !menu.open);  /* then it answers */
    assert(!tick(0, 0));

    /* Circle and the rest pass through while a question is open. */
    assert(!tick(CIRCLE, 1));
    assert_game(CIRCLE, CIRCLE, 0);
    assert(!tick(0, 1));
    assert_game(0, 0, CIRCLE);

    /* A press and release within one tick still reaches the game. */
    {
        uint32_t pressed = SQUARE, released = SQUARE;
        assert(!pw_invite_prompt_tick(&prompt, 0, pressed, released, 1, &view));
        assert(view.held == 0 && view.pressed == SQUARE && view.released == SQUARE);
    }
    assert(!pw_invite_prompt_tick(NULL, 0, 0, 0, 1, &view));
    assert(!pw_invite_prompt_tick(&prompt, 0, 0, 0, 1, NULL));
    return 0;
}
