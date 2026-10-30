/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Host stand-in for the title's controller calls in ntdll.prx
 * (wine/ps5/pw_wine_sink.h), preloaded into a host Wine by
 * tools/test_wine_hid_pad.py so that winebus's PS5 bus starts in winedevice.
 * The pad's state is the text in $PW_PAD_FILE, re-read on every call:
 * "connected buttons left_trigger right_trigger lx ly rx ry" in decimal
 * (buttons are XINPUT_GAMEPAD_* bits), optionally followed by the extras
 * "buttons touching x y" (PW_WINE_PAD_* bits). A missing or unreadable file is a
 * disconnected pad. Each rumble request is appended to $PW_RUMBLE_FILE as
 * "left right". The test program writes the pad file through Wine's Z:
 * drive and reads the rumble file the same way. Controllers 1..3 are the same
 * in "$PW_PAD_FILE.N" and "$PW_RUMBLE_FILE.N", and pw_wine_pads_ask leaves its
 * mask in "$PW_PAD_FILE.asked". */
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

typedef struct PwWinePadExtra {
    uint32_t buttons;
    uint32_t touching;
    uint16_t touch_x,touch_y;
} PwWinePadExtra;

int pw_wine_pad(PwWinePad *pad);
void pw_wine_pad_extra_at(uint32_t index,PwWinePadExtra *extra);
void pw_wine_set_rumble(uint32_t left,uint32_t right);
int pw_wine_pad_peek(uint32_t index,PwWinePad *pad);
void pw_wine_set_rumble_at(uint32_t index,uint32_t left,uint32_t right);
void pw_wine_pads_ask(uint32_t mask);
void pw_wine_pad_notify(void);

int pw_wine_pad_peek(uint32_t index,PwWinePad *pad)
{
    const char *base=getenv("PW_PAD_FILE");
    char path[4096];
    unsigned connected,buttons,lt,rt;
    int lx,ly,rx,ry;
    FILE *f;
    int n;

    if(!base)return 0;
    if(index)snprintf(path,sizeof(path),"%s.%u",base,index);
    else snprintf(path,sizeof(path),"%s",base);
    f=fopen(path,"r");
    n=f?fscanf(f,"%u %u %u %u %d %d %d %d",&connected,&buttons,&lt,&rt,&lx,&ly,&rx,&ry):0;
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

int pw_wine_pad(PwWinePad *pad)
{
    return pw_wine_pad_peek(0,pad);
}

void pw_wine_pad_extra_at(uint32_t index,PwWinePadExtra *extra)
{
    const char *base=getenv("PW_PAD_FILE");
    char path[4096];
    unsigned connected,buttons,lt,rt,extra_buttons,touching,x,y;
    int lx,ly,rx,ry;
    FILE *f;

    extra->buttons=extra->touching=0;
    extra->touch_x=extra->touch_y=0;
    if(!base)return;
    if(index)snprintf(path,sizeof(path),"%s.%u",base,index);
    else snprintf(path,sizeof(path),"%s",base);
    f=fopen(path,"r");
    if(!f)return;
    if(fscanf(f,"%u %u %u %u %d %d %d %d %u %u %u %u",&connected,&buttons,&lt,&rt,&lx,&ly,&rx,&ry,
              &extra_buttons,&touching,&x,&y)==12) {
        extra->buttons=extra_buttons;
        extra->touching=touching;
        extra->touch_x=(uint16_t)x;
        extra->touch_y=(uint16_t)y;
    }
    fclose(f);
}

void pw_wine_set_rumble_at(uint32_t index,uint32_t left,uint32_t right)
{
    const char *base=getenv("PW_RUMBLE_FILE");
    char path[4096];
    FILE *f;

    if(!base)return;
    if(index)snprintf(path,sizeof(path),"%s.%u",base,index);
    else snprintf(path,sizeof(path),"%s",base);
    f=fopen(path,"a");
    if(!f)return;
    fprintf(f,"%u %u\n",left,right);
    fclose(f);
}

void pw_wine_set_rumble(uint32_t left,uint32_t right)
{
    pw_wine_set_rumble_at(0,left,right);
}

void pw_wine_pads_ask(uint32_t mask)
{
    const char *base=getenv("PW_PAD_FILE");
    char path[4096];
    FILE *f;

    if(!base)return;
    snprintf(path,sizeof(path),"%s.asked",base);
    f=fopen(path,"w");
    if(!f)return;
    fprintf(f,"%u\n",mask);
    fclose(f);
}

void pw_wine_pad_notify(void)
{
}
