/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_INPUT_SWITCH_H
#define PW_INPUT_SWITCH_H
#include <stdint.h>

/* Holding one button swaps a title between its two input presets (a
 * launcher's and its game's). The game must not see a hold, so the button is
 * kept from it while down: letting go early passes on a short press instead,
 * which reaches the game when the button comes up. */
enum { PW_INPUT_SWITCH_HOLD_US = 2000000, PW_INPUT_SWITCH_TAP_US = 96000 };

typedef struct PwInputSwitch {
    uint32_t button;
    int down, fired, left;
    uint32_t held_us, tap_us;
    int tap_left;
} PwInputSwitch;

/* The pad as the game should see it this tick. left is where the button
 * counts as pressed (a touchpad's half), switch_now says the hold was reached. */
typedef struct PwInputSwitchView {
    uint32_t held, pressed, released;
    int left, switch_now;
} PwInputSwitchView;

void pw_input_switch_init(PwInputSwitch *input_switch, uint32_t button);
void pw_input_switch_tick(PwInputSwitch *input_switch, uint32_t held, uint32_t pressed, uint32_t released,
                          int left, uint32_t tick_us, PwInputSwitchView *view);
/* The menu or the on-screen keyboard took the pad: no hold or short press goes
 * on. Returns 1 when the game saw a short press that must now come up. */
int pw_input_switch_cancel(PwInputSwitch *input_switch);
#endif
