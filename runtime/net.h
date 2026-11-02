/* Two players over UDP (net.c): each PC runs the whole game; only inputs travel. */
#pragma once
#include <stdint.h>
#include <time.h>

extern int wc_net_on;   /* WC_NET is set */
extern int wc_net_join; /* this is the friend's side: they fly the wingman and only watch the rest */

void net_init(void);    /* before pc_init: reads WC_NET, forces the emulated clock */
void net_connect(void); /* on the emulation's thread, before the game starts */
void net_poll(void);    /* regularly on the emulation's thread: runs the lockstep */

/* Local input, kept back until it is due on both PCs. The capture functions return 1 when they took
 * it; they return 0 outside net mode, and while net.c itself applies an input that is due. */
void net_local_key(uint8_t code);
int net_capture_mouse(int relative, int x, int y, int buttons);
int net_capture_joystick(int x, int y, int roll, int buttons);

/* The friend's controls for this block; 0 when no friend is connected any more. */
int net_friend(int *x, int *y, int *roll, int *throttle, int *fire);
int net_friend_burn(void); /* their afterburner key */
int net_friend_keys(void); /* the steering keys since the last frame: pc.c steps the stick each frame */
void net_frame(uint64_t slot); /* at each frame's slot: the controls that frame goes by */
enum { NET_KEY_LEFT = 1, NET_KEY_RIGHT = 2, NET_KEY_DOWN = 4, NET_KEY_UP = 8, NET_KEY_ROLL_LEFT = 16, NET_KEY_ROLL_RIGHT = 32, NET_KEY_SHIFT = 64 };

/* The friend's controls at that emulated time, for the friend's view running ahead of the game: what
 * is on its way in the blocks, then what they hold now. Returns 1 from where that is what they hold now. */
void net_ahead_prepare(void);
int net_ahead(uint64_t at, int *x, int *y, int *roll, int *throttle, int *fire, int *burn);
int net_ahead_keys(uint64_t at);
uint64_t net_ahead_ns(void); /* how far ahead what they hold now is in force */

/* The next press of T or L, for the friend's view alone; 0 when there is none. */
int net_view_key(void);

time_t net_time(void); /* the same clock on both PCs */

/* Provided by pc.c */
void pc_net_key(uint8_t code);
uint64_t pc_now_ns(void);
