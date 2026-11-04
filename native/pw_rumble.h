/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* How a game's rumble becomes the DualSense's motors, tuned by feel on a
 * controller. */
#ifndef PW_RUMBLE_H
#define PW_RUMBLE_H
#include <stdint.h>

/* A rumble, however short, lasts this long: on the motors, less gets lost. */
enum { PW_RUMBLE_MIN_ON_MS=20 };

/* A motor level 0..255 with weak rumble lifted a little; 0 stays 0. */
uint8_t pw_rumble_motor(uint8_t level);

/* A pad's rumble over time: keeps what the game asked for running for
 * PW_RUMBLE_MIN_ON_MS, including a pulse that started and ended between two
 * reads of the sink (the peak the sink kept). */
typedef struct PwRumbleHold {
    uint64_t until_ms;
    uint32_t held_left,held_right,sent_left,sent_right;
} PwRumbleHold;
/* One tick. changed, left and right are what the sink reported, peak_left
 * and peak_right the strongest since its last read. Sets out_left and
 * out_right to what the motors should run at; returns 1 when that needs
 * sending. */
int pw_rumble_hold_step(PwRumbleHold *,uint64_t now_ms,int changed,
                        uint32_t left,uint32_t right,uint32_t peak_left,uint32_t peak_right,
                        uint32_t *out_left,uint32_t *out_right);
#endif
