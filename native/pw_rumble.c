/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_rumble.h"

/* What a level of 128 becomes: two lines, (0,0) to (128,MID) and on to (255,255). */
enum { MID=140 };

uint8_t pw_rumble_motor(uint8_t level)
{
    uint32_t out=level<=128?(uint32_t)level*MID/128:MID+((uint32_t)level-128)*(255-MID)/127;
    return (uint8_t)out;
}

int pw_rumble_hold_step(PwRumbleHold *hold,uint64_t now_ms,int changed,
                        uint32_t left,uint32_t right,uint32_t peak_left,uint32_t peak_right,
                        uint32_t *out_left,uint32_t *out_right)
{
    if(now_ms>=hold->until_ms)hold->held_left=hold->held_right=0;
    if(changed) {
        uint32_t want_left=peak_left>left?peak_left:left,want_right=peak_right>right?peak_right:right;
        /* More than the motors run at starts the time again. A rumble that
         * fades or ends does not: the peak then is only where it was. */
        if(want_left>hold->sent_left || want_right>hold->sent_right) {
            if(want_left>hold->held_left)hold->held_left=want_left;
            if(want_right>hold->held_right)hold->held_right=want_right;
            hold->until_ms=now_ms+PW_RUMBLE_MIN_ON_MS;
        }
    }
    *out_left=left>hold->held_left?left:hold->held_left;
    *out_right=right>hold->held_right?right:hold->held_right;
    if(*out_left==hold->sent_left && *out_right==hold->sent_right)return 0;
    hold->sent_left=*out_left;hold->sent_right=*out_right;
    return 1;
}
