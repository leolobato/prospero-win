/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_rumble.h"
#include <assert.h>

int main(void)
{
    uint32_t left,right;

    /* Motors: 0 stays 0, a running motor never reads 0, weak rumble is lifted, full stays full. */
    assert(pw_rumble_motor(0)==0 && pw_rumble_motor(1)==1 && pw_rumble_motor(255)==255);
    assert(pw_rumble_motor(128)==140 && pw_rumble_motor(64)==70);
    for(int level=1;level<256;level++)assert(pw_rumble_motor((uint8_t)level)>=pw_rumble_motor((uint8_t)(level-1)));

    /* A pulse lost between reads runs for the hold, then stops, once each. */
    PwRumbleHold hold={0};
    assert(!pw_rumble_hold_step(&hold,0,0,0,0,0,0,&left,&right));
    assert(pw_rumble_hold_step(&hold,1000,1,0,0,60000,100,&left,&right)==1 && left==60000 && right==100);
    assert(!pw_rumble_hold_step(&hold,1016,0,0,0,0,0,&left,&right) && left==60000);
    assert(pw_rumble_hold_step(&hold,1000+PW_RUMBLE_MIN_ON_MS,0,0,0,0,0,&left,&right)==1 && !left && !right);
    assert(!pw_rumble_hold_step(&hold,1040,0,0,0,0,0,&left,&right));

    /* A long rumble runs as asked, and a stronger one during a hold takes over. */
    hold=(PwRumbleHold){0};
    assert(pw_rumble_hold_step(&hold,0,1,1000,0,1000,0,&left,&right)==1 && left==1000);
    assert(pw_rumble_hold_step(&hold,5,1,2000,0,2000,0,&left,&right)==1 && left==2000);
    assert(!pw_rumble_hold_step(&hold,10,1,500,0,2000,0,&left,&right) && left==2000);
    assert(pw_rumble_hold_step(&hold,30,0,500,0,500,0,&left,&right)==1 && left==500);
    /* It ends when asked: its own level as the peak is not a new pulse. */
    assert(pw_rumble_hold_step(&hold,40,1,0,0,500,0,&left,&right)==1 && !left);
    return 0;
}
