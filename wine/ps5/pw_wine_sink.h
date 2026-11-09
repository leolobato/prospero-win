/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_SINK_H
#define PW_WINE_SINK_H
#include <stdint.h>
#include "menu/pw_title_menu.h"
#include "menu/pw_title_notify.h"

/* The meeting point between the title and Wine's PS5 user driver. ntdll
 * links it: the title reaches the set/post calls through ntdll.prx's
 * descriptor before __wine_main, and win32u's driver reaches the present
 * and next calls with dlsym. Frames go out through one sink, the title's
 * pw_present_frame path; input comes in through a bounded queue the title
 * fills from the DualSense and the driver drains in ProcessEvents. A
 * game's XInput controller is one more slot: the title keeps the newest
 * gamepad state in it, Wine's xinput reads it (its Unix library, patch
 * 0470, finds these calls with dlsym) and leaves the rumble it asks for;
 * the driver tells the game's windows when it comes or goes (patch 0984).
 * The title's in-game menu is another slot: the title stores what to draw
 * and ntdll hands it to the game's renderer (menu/pw_title_menu.h).
 * Code in the game also asks the title for a system notification and for
 * an invite question the player answers with L3+R3 (menu/pw_title_notify.h).
 * A game that presents with Vulkan scans out itself (patch 0460): the
 * driver asks the title once to release the video output. Frames still
 * reach the title after that, which shows them through the Vulkan driver
 * while no swapchain presents (a DirectShow movie drawn with GDI).
 * Sound goes out like frames: Wine's audio driver (wine/wineps5) hands its
 * mix to the title's one audio port, one grain at a time. */

/* BGRA, top-down; stride in bytes. Returns the sink's result. */
typedef int (*PwWinePresentSink)(void *context,const void *bgra,uint32_t width,uint32_t height,
                                 uint32_t stride);

enum { PW_WINE_INPUT_KEY=1,PW_WINE_INPUT_MOUSE_MOVE,PW_WINE_INPUT_MOUSE_BUTTON,
       PW_WINE_INPUT_MOUSE_WHEEL,
       PW_WINE_INPUT_QUEUE=256 };
typedef struct PwWineInput {
    uint32_t type;
    uint32_t code;      /* KEY: Windows virtual key; MOUSE_BUTTON: 0 left, 1 right, 2 middle */
    int32_t x,y;        /* MOUSE_MOVE: relative pixels; MOUSE_WHEEL: x = signed detents (up positive) */
    uint32_t down;      /* KEY, MOUSE_BUTTON: 1 pressed, 0 released */
} PwWineInput;

/* The console's output format: interleaved left,right 16-bit frames at
 * 48 kHz, played PW_WINE_AUDIO_GRAIN frames at a time. */
enum { PW_WINE_AUDIO_RATE=48000,PW_WINE_AUDIO_GRAIN=256 };
/* Plays one grain and returns once the console has taken it, so the
 * console's output clocks the caller. Returns the sink's result. */
typedef int (*PwWineAudioSink)(void *context,const int16_t *frames);

/* Hands the video output over to a Vulkan display-plane surface; 0 once
 * the title no longer uses it. */
typedef int (*PwWineDisplayRelease)(void *context);

/* Shows text (UTF-8, NUL-terminated) as a system notification; 0 when it
 * did, a positive value when it did not. It runs on the game's thread and
 * must not wait. */
typedef int (*PwWineNotify)(void *context,const char *text);

typedef struct PwWineSinkStats {
    uint64_t frames,frames_dropped;       /* dropped: no sink set or it failed */
    uint64_t inputs_posted,inputs_dropped,inputs_delivered;
    uint64_t grains,grains_dropped;       /* audio; dropped as for frames */
    uint32_t display_released;            /* 1 once Vulkan took the video output */
} PwWineSinkStats;

/* XInput's controllers (XUSER_MAX_COUNT): 0 is the title's own user's
 * DualSense, 1..3 the other signed-in users'. */
enum { PW_WINE_PADS=4 };

/* An XInput gamepad (XINPUT_GAMEPAD with the connection and a packet
 * number in front), laid out as xinput's Unix library reads it. */
typedef struct PwWinePad {
    uint32_t connected;     /* 0: no controller */
    uint32_t packet;        /* the sink's: changes whenever the state does */
    uint16_t buttons;       /* XINPUT_GAMEPAD_* */
    uint8_t left_trigger,right_trigger;
    int16_t thumb_lx,thumb_ly,thumb_rx,thumb_ry;
} PwWinePad;

/* What a PlayStation controller has beside an XInput gamepad: the
 * touchpad's click, Share (the DualSense's Create) and the first finger on
 * the touchpad. The title keeps it next to the gamepad when the game sees
 * its controller as a DualShock 4 (WINE_PS5_PAD=dualshock4); winebus's PS5
 * bus then reads both. */
enum { PW_WINE_PAD_TOUCHPAD=0x1,PW_WINE_PAD_SHARE=0x2 };
enum { PW_WINE_PAD_TOUCH_WIDTH=1920,PW_WINE_PAD_TOUCH_HEIGHT=1080 };
typedef struct PwWinePadExtra {
    uint32_t buttons;       /* PW_WINE_PAD_TOUCHPAD, PW_WINE_PAD_SHARE */
    uint32_t touching;      /* 1 while a finger is on the touchpad */
    uint16_t touch_x,touch_y;   /* that finger, 0..PW_WINE_PAD_TOUCH_WIDTH/HEIGHT-1 */
} PwWinePadExtra;

/* Title side. */
void pw_wine_set_present_sink(PwWinePresentSink sink,void *context);
void pw_wine_set_audio_sink(PwWineAudioSink sink,void *context);
void pw_wine_set_display_release(PwWineDisplayRelease release,void *context);
/* 0, or -1 when the queue is full or the event is invalid. */
int pw_wine_post_input(const PwWineInput *event);
void pw_wine_sink_stats(PwWineSinkStats *stats);
/* The gamepad's newest state, or NULL when it is disconnected. The sink
 * numbers the packets; the pad's own packet field is ignored. */
void pw_wine_set_pad(const PwWinePad *pad);
/* The gamepad's extras, or NULL for none (all released, no finger).
 * Coordinates are clamped. */
void pw_wine_set_pad_extra(const PwWinePadExtra *extra);
void pw_wine_set_pad_extra_at(uint32_t index,const PwWinePadExtra *extra);
/* 1 and the newest motor speeds (0..65535) when they changed since the
 * last call, else 0. */
int pw_wine_rumble(uint32_t *left,uint32_t *right);
/* The same for controller index (< PW_WINE_PADS; others are ignored and
 * read as disconnected); the calls above are controller 0's. */
void pw_wine_set_pad_at(uint32_t index,const PwWinePad *pad);
int pw_wine_rumble_at(uint32_t index,uint32_t *left,uint32_t *right);
/* The same, with the strongest speeds set since the last read: a pulse the
 * game started and ended between two reads shows only here. */
int pw_wine_rumble_peak_at(uint32_t index,uint32_t *left,uint32_t *right,uint32_t *peak_left,uint32_t *peak_right);
/* Bit i set once the game asked about controller i (pw_wine_pad_at,
 * pw_wine_pads_ask); the title opens the other users' pads only then. */
uint32_t pw_wine_pads_wanted(void);
/* The menu to draw, or NULL to close it. The sink sets version and
 * sequence, clamps lines and selected, and terminates every string. */
void pw_wine_set_menu(const struct pw_title_menu *menu);
void pw_wine_set_notify(PwWineNotify notify,void *context);
/* 1: the title answers invite questions (below); 0 forgets an open one and
 * refuses new ones. Times are CLOCK_MONOTONIC milliseconds, read by the
 * caller, so the title and ntdll measure the same deadline. */
void pw_wine_set_invite(uint32_t enabled);
/* 1 while a question is open and its deadline has not passed. Once it has,
 * the question is answered PW_TITLE_INVITE_EXPIRED. *generation (optional)
 * names the question: a new question, or forgetting one, changes it. */
int pw_wine_invite_pending(uint64_t now_ms,uint64_t *generation);
/* Answers the open question PW_TITLE_INVITE_ACCEPTED: 1 when it is still
 * the one pending named (generation) and still in time, else 0, so a press
 * meant for a question replaced meanwhile accepts nothing. */
int pw_wine_invite_accept(uint64_t now_ms,uint64_t generation);

/* Driver side. -1 when no sink is set or the arguments are invalid. */
int pw_wine_present(const void *bgra,uint32_t width,uint32_t height,uint32_t stride);
/* Asks the title to release the video output, once: 0 when it did (or has
 * nothing to release), else the title's refusal, and it may be asked again. */
int pw_wine_release_display(void);
/* 1 and the oldest event, or 0 when the queue is empty. */
int pw_wine_next_input(PwWineInput *event);
/* A non-blocking pipe that becomes readable when input is posted, so the
 * driver can have the server wake a thread idle in GetMessage. Readers
 * drain it before emptying the queue, which loses no wakeup. -1 if no
 * pipe could be created. */
int pw_wine_input_fd(void);
/* 1 and the gamepad's state while one is connected, else 0. */
int pw_wine_pad(PwWinePad *pad);
/* The newest extras: zeros until the title sets them. */
void pw_wine_pad_extra(PwWinePadExtra *extra);
void pw_wine_pad_extra_at(uint32_t index,PwWinePadExtra *extra);
/* The newest menu snapshot; a closed menu reads as zeros apart from
 * version and sequence. */
void pw_wine_menu(struct pw_title_menu *menu);
/* The title's notify sink's result (0 shown, positive not), or -1 without
 * a sink or without text. */
int pw_wine_notify(const char *text);
/* Opens a question the player has timeout_ms to accept, replacing an open
 * one or an answer not yet read; 0 forgets both. 0, or -1 when the title
 * takes no questions. */
int pw_wine_invite_ask(uint32_t timeout_ms,uint64_t now_ms);
/* PW_TITLE_INVITE_NONE while a question is open or none was asked, else its
 * answer, once; -1 when the title takes no questions. */
int pw_wine_invite_answer(uint64_t now_ms);

/* How many times the gamepad connected or disconnected. Each time also
 * wakes the input fd, so the driver can tell the game's windows that a
 * device came or went (WM_DEVICECHANGE), as Windows does. */
uint32_t pw_wine_pad_changes(void);
/* The rumble a game asked for (XInputSetState); speeds are clamped. */
void pw_wine_set_rumble(uint32_t left,uint32_t right);
int pw_wine_pad_at(uint32_t index,PwWinePad *pad);
/* The same without noting the controller as asked for: winebus reads every
 * controller in every game. */
int pw_wine_pad_peek(uint32_t index,PwWinePad *pad);
/* A game looked for the controllers in mask some other way than XInput. */
void pw_wine_pads_ask(uint32_t mask);
/* Tell the game's windows that the controllers changed, as a connection does. */
void pw_wine_pad_notify(void);
void pw_wine_set_rumble_at(uint32_t index,uint32_t left,uint32_t right);
/* 1 when the title set an audio sink: the audio driver offers its device
 * only then. */
int pw_wine_audio_available(void);
/* Blocks while the sink plays one grain. -1 when no sink is set or frames
 * is NULL. */
int pw_wine_audio_output(const int16_t *frames);

#endif
