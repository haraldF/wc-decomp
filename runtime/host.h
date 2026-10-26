/* The host platform around the emulated PC: a window with the VGA picture and the keyboard
 * (host_sdl.c; host_web.c in the browser). The emulation never includes SDL; it runs on a thread of its own while the host
 * owns the main thread, which macOS needs for windows (and Emscripten for the canvas). */
#pragma once
#include <stdint.h>

/* Open the window, unless WC_HEADLESS is set or there is no display. Returns 0 when headless. */
int host_open(void);
/* Run game on a thread of its own and serve the window until the program exits. */
_Noreturn void host_run(void (*game)(void));

/* Provided by the emulation, called on the host's thread. */
/* The newest complete frame: mode 13h pixels and the 6-bit palette, taken at vertical retrace.
 * Returns a count that changes with every frame. */
uint64_t vga_frame(uint8_t pixels[320 * 200], uint8_t palette[256 * 3]);
/* A scancode (set 1: make code, | 0x80 for the break code) from the keyboard. */
void pc_key(uint8_t scancode);
/* CPU speed in instructions per millisecond (0: unlimited); change > 0 raises it by 20%, < 0
 * lowers it, 0 only reads it. */
uint64_t pc_cycles(int change);
/* The frame rate in space flight (0: not paced by frames); pc_cycles changes it along. */
uint64_t pc_flight_fps(void);
/* The mouse over the picture: x 0..319, y 0..199; buttons: 1 left, 2 right, 4 middle. */
void pc_mouse(int x, int y, int buttons);
/* Mouse movement by dx, dy picture pixels from wherever the pointer is (full screen). */
void pc_mouse_move(int dx, int dy, int buttons);
/* Provided by the host: the program ends with this code (DOS exit, or an error), on the emulation's
 * thread. */
_Noreturn void host_exit(int code);
/* Provided by the host: whether there is a mouse (a window, or WC_MOUSE=1 for tests). */
int host_has_mouse(void);
/* A file in the game folder was written (on closing it) or deleted: the web host keeps such files
 * (save games) across sessions. Called on the emulation's thread. */
void host_file_saved(const char *path);
/* Sound: 16-bit stereo at rate. host_audio_open returns 0 when there is no sound output. */
int host_audio_open(unsigned rate);
void host_audio(const int16_t *frames, unsigned count);
