/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Host stand-in for the title's controller calls in ntdll.prx
 * (wine/ps5/pw_wine_sink.h), preloaded into a host Wine by
 * tools/test_wine_hid_pad.py so that winebus's PS5 bus starts in winedevice.
 * The pad's state is the text in $PW_PAD_FILE, re-read on every call:
 * "connected buttons left_trigger right_trigger lx ly rx ry" in decimal
 * (buttons are XINPUT_GAMEPAD_* bits). A missing or unreadable file is a
 * disconnected pad. Each rumble request is appended to $PW_RUMBLE_FILE as
 * "left right". The test program writes the pad file through Wine's Z:
 * drive and reads the rumble file the same way. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct PwWinePad {
    uint32_t connected;
    uint32_t packet;
    uint16_t buttons;
    uint8_t left_trigger,right_trigger;
    int16_t thumb_lx,thumb_ly,thumb_rx,thumb_ry;
} PwWinePad;

int pw_wine_pad(PwWinePad *pad);
void pw_wine_set_rumble(uint32_t left,uint32_t right);

int pw_wine_pad(PwWinePad *pad)
{
    const char *path=getenv("PW_PAD_FILE");
    unsigned connected,buttons,lt,rt;
    int lx,ly,rx,ry;
    FILE *f=path?fopen(path,"r"):NULL;
    int n=f?fscanf(f,"%u %u %u %u %d %d %d %d",&connected,&buttons,&lt,&rt,&lx,&ly,&rx,&ry):0;
    if(f)fclose(f);
    if(n!=8 || !connected)return 0;
    pad->connected=1;
    pad->packet=0;
    pad->buttons=(uint16_t)buttons;
    pad->left_trigger=(uint8_t)lt;
    pad->right_trigger=(uint8_t)rt;
    pad->thumb_lx=(int16_t)lx;
    pad->thumb_ly=(int16_t)ly;
    pad->thumb_rx=(int16_t)rx;
    pad->thumb_ry=(int16_t)ry;
    return 1;
}

void pw_wine_set_rumble(uint32_t left,uint32_t right)
{
    const char *path=getenv("PW_RUMBLE_FILE");
    FILE *f=path?fopen(path,"a"):NULL;
    if(!f)return;
    fprintf(f,"%u %u\n",left,right);
    fclose(f);
}
