/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_input_switch.h"
#include <assert.h>
#include <stdio.h>

enum { TOUCH = 0x100000, CROSS = 0x4000, TICK = 16000 };

static PwInputSwitchView tick(PwInputSwitch *s, uint32_t held, uint32_t pressed, uint32_t released, int left)
{
    PwInputSwitchView view;

    pw_input_switch_tick(s, held, pressed, released, left, TICK, &view);
    return view;
}

int main(void)
{
    PwInputSwitch s;
    PwInputSwitchView v;
    int switches = 0;

    /* Other buttons pass through, whatever the touchpad does. */
    pw_input_switch_init(&s, TOUCH);
    v = tick(&s, CROSS, CROSS, 0, 0);
    assert(v.held == CROSS && v.pressed == CROSS && !v.released && !v.switch_now);
    v = tick(&s, CROSS | TOUCH, TOUCH, 0, 1);
    assert(v.held == CROSS && !v.pressed && !v.released);

    /* A hold is never seen, swaps once, and its release isn't seen either. */
    for (uint32_t t = 0; t < PW_INPUT_SWITCH_HOLD_US + 500000; t += TICK) {
        v = tick(&s, TOUCH, 0, 0, 1);
        assert(!v.held && !v.pressed && !v.released);
        switches += v.switch_now;
    }
    assert(switches == 1);
    v = tick(&s, 0, 0, TOUCH, 1);
    assert(!v.held && !v.pressed && !v.released && !v.switch_now);
    v = tick(&s, 0, 0, 0, 0);
    assert(!v.held && !v.pressed && !v.released);

    /* Not a moment before the hold time. */
    pw_input_switch_init(&s, TOUCH);
    tick(&s, TOUCH, TOUCH, 0, 0);
    for (uint32_t t = TICK; t < PW_INPUT_SWITCH_HOLD_US; t += TICK) assert(!tick(&s, TOUCH, 0, 0, 0).switch_now);
    assert(tick(&s, TOUCH, 0, 0, 0).switch_now);

    /* A short press reaches the game when it ends, on the half it began. */
    pw_input_switch_init(&s, TOUCH);
    v = tick(&s, TOUCH, TOUCH, 0, 1);
    assert(!v.held && !v.pressed);
    v = tick(&s, TOUCH, 0, 0, 0);
    assert(!v.held);
    v = tick(&s, 0, 0, TOUCH, 0);
    assert(v.held == TOUCH && v.pressed == TOUCH && !v.released && v.left == 1);
    int ticks = 0;
    do {
        v = tick(&s, 0, 0, 0, 0);
        if (!v.released) assert(v.held == TOUCH && !v.pressed && v.left == 1);
        ticks++;
    } while (!v.released);
    assert(v.released == TOUCH && !v.held && !v.pressed);
    assert(ticks * TICK >= PW_INPUT_SWITCH_TAP_US && ticks < 20);
    v = tick(&s, 0, 0, 0, 0);
    assert(!v.held && !v.released);

    /* Down and up within one batch is a short press too. */
    pw_input_switch_init(&s, TOUCH);
    v = tick(&s, 0, TOUCH, TOUCH, 1);
    assert(v.pressed == TOUCH && v.held == TOUCH && !v.released && v.left == 1);

    /* A new press ends the one being passed on, and is timed from its start. */
    pw_input_switch_init(&s, TOUCH);
    tick(&s, TOUCH, TOUCH, 0, 0);
    tick(&s, 0, 0, TOUCH, 0);
    v = tick(&s, TOUCH, TOUCH, 0, 1);
    assert(v.released == TOUCH && !v.held && !v.pressed);
    assert(!tick(&s, TOUCH, 0, 0, 1).switch_now);

    /* The pad taken away mid-press: no release comes, and no press is invented. */
    pw_input_switch_init(&s, TOUCH);
    tick(&s, TOUCH, TOUCH, 0, 0);
    v = tick(&s, 0, 0, 0, 0);
    assert(!v.held && !v.pressed && !v.released);
    v = tick(&s, 0, 0, 0, 0);
    assert(!v.held && !v.pressed && !v.released);

    /* Cancelled (the menu opened): a hold never swaps, and its release is no press. */
    pw_input_switch_init(&s, TOUCH);
    tick(&s, TOUCH, TOUCH, 0, 0);
    assert(!pw_input_switch_cancel(&s));
    v = tick(&s, 0, 0, TOUCH, 0);
    assert(!v.held && !v.pressed && !v.released);
    /* A short press under way is reported, to be let up. */
    pw_input_switch_init(&s, TOUCH);
    tick(&s, TOUCH, TOUCH, 0, 0);
    tick(&s, 0, 0, TOUCH, 0);
    assert(pw_input_switch_cancel(&s));
    v = tick(&s, 0, 0, 0, 0);
    assert(!v.held && !v.released);

    puts("input switch passed: a hold swaps once unseen, a short press reaches the game on release");
    return 0;
}
