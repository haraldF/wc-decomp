/* The host in the browser (Emscripten). The page (web/index.html) owns the canvas, the keyboard,
 * the mouse and the sound; it calls the web_* functions here, and plays the samples host_audio()
 * puts into a ring buffer in the (shared) memory with an AudioWorklet. The emulation runs on a
 * pthread (a Web Worker sharing the memory); the main thread returns to the browser. */
#include <emscripten.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "host.h"

int host_open(void)
{
	return 1;
}

int host_has_mouse(void)
{
	return 1;
}

/* ---- the picture: the page asks for it at every animation frame ---- */

/* The newest frame as RGBA bytes (320x200), or NULL when it hasn't changed since the last call. */
EMSCRIPTEN_KEEPALIVE uint32_t *web_frame(void)
{
	static uint8_t pixels[320 * 200], palette[256 * 3];
	static uint32_t rgba[320 * 200];
	static uint64_t shown = ~0ull;
	uint64_t n = vga_frame(pixels, palette);
	if (n == shown) return NULL;
	shown = n;
	uint32_t colour[256];
	for (int i = 0; i < 256; i++) {
		const uint8_t *c = palette + 3 * i; /* 6 bits per component to 8; R, G, B, A in memory */
		colour[i] = 0xff000000u | (uint32_t)((c[2] << 2) | (c[2] >> 4)) << 16 |
			(uint32_t)((c[1] << 2) | (c[1] >> 4)) << 8 | (uint32_t)((c[0] << 2) | (c[0] >> 4));
	}
	for (int i = 0; i < 320 * 200; i++) rgba[i] = colour[pixels[i]];
	return rgba;
}

/* ---- input and speed, from the page ---- */

EMSCRIPTEN_KEEPALIVE void web_key(int scancode)
{
	pc_key((uint8_t)scancode);
}

EMSCRIPTEN_KEEPALIVE void web_mouse(int x, int y, int buttons)
{
	pc_mouse(x, y, buttons);
}

EMSCRIPTEN_KEEPALIVE void web_mouse_move(int dx, int dy, int buttons)
{
	pc_mouse_move(dx, dy, buttons);
}

EMSCRIPTEN_KEEPALIVE void web_joystick(int x, int y, int roll, int buttons)
{
	pc_joystick(x, y, roll, buttons);
}

/* Instructions per ms (0: unlimited) and the frame rate in flight; change as for pc_cycles. */
EMSCRIPTEN_KEEPALIVE double web_cycles(int change)
{
	return (double)pc_cycles(change);
}

EMSCRIPTEN_KEEPALIVE double web_flight_fps(int change)
{
	return (double)pc_flight_fps(change);
}

EMSCRIPTEN_KEEPALIVE int web_in_flight(void)
{
	return pc_in_flight();
}

/* ---- sound: a ring buffer of 16-bit stereo frames that the page's AudioWorklet reads ---- */

enum { AUDIO_FRAMES = 1 << 16 };
static int16_t audio_ring[AUDIO_FRAMES * 2];
/* Read by the worklet as 32-bit words: ring address, its size in frames, frames written, frames
 * read (the worklet advances it), sample rate (0 until the sound starts). */
static struct {
	uint32_t ring, frames;
	_Atomic uint32_t written, read, rate;
} audio_info;

EMSCRIPTEN_KEEPALIVE void *web_audio_info(void)
{
	audio_info.ring = (uint32_t)(uintptr_t)audio_ring;
	audio_info.frames = AUDIO_FRAMES;
	return &audio_info;
}

int host_audio_open(unsigned rate)
{
	web_audio_info();
	atomic_store(&audio_info.rate, rate);
	return 1;
}

/* Called on the emulation's thread. When the page doesn't play (yet), the ring fills up and the
 * newest samples are dropped; the worklet skips ahead when it falls behind. */
void host_audio(const int16_t *frames, unsigned count)
{
	uint32_t w = atomic_load_explicit(&audio_info.written, memory_order_relaxed);
	uint32_t r = atomic_load_explicit(&audio_info.read, memory_order_acquire);
	for (unsigned i = 0; i < count && w - r < AUDIO_FRAMES; i++, w++) {
		audio_ring[(w % AUDIO_FRAMES) * 2] = frames[i * 2];
		audio_ring[(w % AUDIO_FRAMES) * 2 + 1] = frames[i * 2 + 1];
	}
	atomic_store_explicit(&audio_info.written, w, memory_order_release);
}

/* ---- files: what the game writes goes to the page, which keeps it in IndexedDB ---- */

void host_file_saved(const char *path)
{
	MAIN_THREAD_EM_ASM({ if (Module.fileSaved) Module.fileSaved(UTF8ToString($0)); }, path);
}

/* ---- threads ---- */

/* The game exited (to DOS, or with an error). exit() wouldn't end the program, since the runtime is
 * kept alive (host_run), nor tell the page: so end this thread and tell the page itself. */
_Noreturn void host_exit(int code)
{
	fflush(stdout);
	fflush(stderr);
	fsync(STDOUT_FILENO); /* the console prints lines: also the last one without a newline */
	MAIN_THREAD_EM_ASM({ if (Module.gameExited) Module.gameExited($0); }, code);
	pthread_exit(NULL);
}

static void *game_thread(void *arg)
{
	((void (*)(void))arg)();
	host_exit(0);
}

_Noreturn void host_run(void (*game)(void))
{
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 16u << 20); /* the game nests C calls only a few KB deep */
	pthread_t thread;
	if (pthread_create(&thread, &attr, game_thread, (void *)game)) {
		fprintf(stderr, "wc: cannot start the game thread\n");
		exit(1);
	}
	emscripten_exit_with_live_runtime(); /* main returns to the browser; the game goes on */
}
