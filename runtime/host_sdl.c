/* The host with SDL3: a window showing the VGA picture at 4:3, the keyboard as PC scancodes, the
 * mouse for the mouse driver (mouse.c).
 * Cmd+F toggles full screen, Cmd+Up/Down change the CPU speed, Cmd+Left/Right the mouse's speed
 * in full screen (both shown in the title), Cmd+Q or closing the window quits. WC_HEADLESS=1: no
 * window. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL3/SDL.h>

#include "host.h"

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *picture; /* 320x200, the frame in colour */
static SDL_Texture *scaled;  /* the picture enlarged by whole factors, then smoothly to the window */
static int scaled_x, scaled_y;
static SDL_FRect shown_at;    /* where the picture is in the window (render pixels), for the mouse */
/* Captured (full screen) movement times this: 1 crosses the picture with a movement as wide as the
 * picture on the screen, which was too slow (WC_MOUSE_SPEED; Cmd+Left/Right change it). */
static float mouse_speed = 2;
static SDL_AudioStream *audio;
static unsigned audio_rate;

int host_has_mouse(void)
{
	const char *m = getenv("WC_MOUSE");
	return window != NULL || (m && *m && *m != '0');
}

int host_open(void)
{
	const char *headless = getenv("WC_HEADLESS");
	if (headless && *headless && *headless != '0') return 0;
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "wc: no window (%s); running headless\n", SDL_GetError());
		return 0;
	}
	if (!SDL_CreateWindowAndRenderer("Wing Commander", 960, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
		    &window, &renderer)) {
		fprintf(stderr, "wc: no window (%s); running headless\n", SDL_GetError());
		SDL_Quit();
		return 0;
	}
	SDL_SetRenderVSync(renderer, 1);
	const char *speed = getenv("WC_MOUSE_SPEED");
	if (speed && atof(speed) > 0) mouse_speed = (float)atof(speed);
	picture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 200);
	SDL_SetTextureScaleMode(picture, SDL_SCALEMODE_NEAREST);
	return 1;
}

/* ---- keyboard: SDL scancodes (USB HID positions) to PC set 1 ---- */

static uint8_t pc_scancode[SDL_SCANCODE_COUNT];
static uint8_t held[SDL_SCANCODE_COUNT];

static void init_keys(void)
{
	static const struct { SDL_Scancode sdl; uint8_t pc; } map[] = {
		{ SDL_SCANCODE_ESCAPE, 0x01 }, { SDL_SCANCODE_MINUS, 0x0c }, { SDL_SCANCODE_EQUALS, 0x0d },
		{ SDL_SCANCODE_BACKSPACE, 0x0e }, { SDL_SCANCODE_TAB, 0x0f }, { SDL_SCANCODE_LEFTBRACKET, 0x1a },
		{ SDL_SCANCODE_RIGHTBRACKET, 0x1b }, { SDL_SCANCODE_RETURN, 0x1c }, { SDL_SCANCODE_LCTRL, 0x1d },
		{ SDL_SCANCODE_RCTRL, 0x1d }, { SDL_SCANCODE_SEMICOLON, 0x27 }, { SDL_SCANCODE_APOSTROPHE, 0x28 },
		{ SDL_SCANCODE_GRAVE, 0x29 }, { SDL_SCANCODE_LSHIFT, 0x2a }, { SDL_SCANCODE_BACKSLASH, 0x2b },
		{ SDL_SCANCODE_COMMA, 0x33 }, { SDL_SCANCODE_PERIOD, 0x34 }, { SDL_SCANCODE_SLASH, 0x35 },
		{ SDL_SCANCODE_RSHIFT, 0x36 }, { SDL_SCANCODE_KP_MULTIPLY, 0x37 }, { SDL_SCANCODE_LALT, 0x38 },
		{ SDL_SCANCODE_RALT, 0x38 }, { SDL_SCANCODE_SPACE, 0x39 }, { SDL_SCANCODE_CAPSLOCK, 0x3a },
		{ SDL_SCANCODE_NUMLOCKCLEAR, 0x45 }, { SDL_SCANCODE_SCROLLLOCK, 0x46 },
		{ SDL_SCANCODE_KP_7, 0x47 }, { SDL_SCANCODE_KP_8, 0x48 }, { SDL_SCANCODE_KP_9, 0x49 },
		{ SDL_SCANCODE_KP_MINUS, 0x4a }, { SDL_SCANCODE_KP_4, 0x4b }, { SDL_SCANCODE_KP_5, 0x4c },
		{ SDL_SCANCODE_KP_6, 0x4d }, { SDL_SCANCODE_KP_PLUS, 0x4e }, { SDL_SCANCODE_KP_1, 0x4f },
		{ SDL_SCANCODE_KP_2, 0x50 }, { SDL_SCANCODE_KP_3, 0x51 }, { SDL_SCANCODE_KP_0, 0x52 },
		{ SDL_SCANCODE_KP_PERIOD, 0x53 }, { SDL_SCANCODE_KP_ENTER, 0x1c }, { SDL_SCANCODE_KP_DIVIDE, 0x35 },
		{ SDL_SCANCODE_F11, 0x57 }, { SDL_SCANCODE_F12, 0x58 },
		/* the grey keys as their keypad twins (no E0 prefix), which every DOS game understands */
		{ SDL_SCANCODE_HOME, 0x47 }, { SDL_SCANCODE_UP, 0x48 }, { SDL_SCANCODE_PAGEUP, 0x49 },
		{ SDL_SCANCODE_LEFT, 0x4b }, { SDL_SCANCODE_RIGHT, 0x4d }, { SDL_SCANCODE_END, 0x4f },
		{ SDL_SCANCODE_DOWN, 0x50 }, { SDL_SCANCODE_PAGEDOWN, 0x51 }, { SDL_SCANCODE_INSERT, 0x52 },
		{ SDL_SCANCODE_DELETE, 0x53 },
	};
	for (size_t i = 0; i < sizeof map / sizeof *map; i++) pc_scancode[map[i].sdl] = map[i].pc;
	static const char rows[] = "1234567890\0\0\0\0qwertyuiop\0\0\0\0asdfghjkl\0\0\0\0\0zxcvbnm";
	for (size_t i = 0; i < sizeof rows - 1; i++) {
		char c = rows[i];
		if (c >= 'a' && c <= 'z') pc_scancode[SDL_SCANCODE_A + (c - 'a')] = (uint8_t)(0x02 + i);
		else if (c >= '1' && c <= '9') pc_scancode[SDL_SCANCODE_1 + (c - '1')] = (uint8_t)(0x02 + i);
		else if (c == '0') pc_scancode[SDL_SCANCODE_0] = (uint8_t)(0x02 + i);
	}
	for (int i = 0; i < 10; i++) pc_scancode[SDL_SCANCODE_F1 + i] = (uint8_t)(0x3b + i);
}

static void show_speed(void)
{
	char title[120];
	uint64_t c = pc_cycles(0);
	if (c) snprintf(title, sizeof title, "Wing Commander \xe2\x80\x94 %llu cycles/ms, %llu fps in flight, mouse \xc3\x97%.2g",
		(unsigned long long)c, (unsigned long long)pc_flight_fps(), mouse_speed);
	else snprintf(title, sizeof title, "Wing Commander \xe2\x80\x94 max speed, mouse \xc3\x97%.2g", mouse_speed);
	SDL_SetWindowTitle(window, title);
}

static void key(const SDL_KeyboardEvent *e)
{
	if (e->mod & SDL_KMOD_GUI) { /* Cmd: for the Mac, not the game */
		if (e->down && !e->repeat && e->scancode == SDL_SCANCODE_F) {
			SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
		}
		if (e->down && (e->scancode == SDL_SCANCODE_LEFT || e->scancode == SDL_SCANCODE_RIGHT)) {
			mouse_speed *= e->scancode == SDL_SCANCODE_RIGHT ? 1.25f : 0.8f;
			mouse_speed = SDL_clamp(mouse_speed, 0.25f, 16.0f);
			fprintf(stderr, "wc: mouse speed %.2g (WC_MOUSE_SPEED)\n", mouse_speed); /* full screen has no title */
			show_speed();
		}
		if (e->down && (e->scancode == SDL_SCANCODE_UP || e->scancode == SDL_SCANCODE_DOWN)) {
			pc_cycles(e->scancode == SDL_SCANCODE_UP ? 1 : -1);
			show_speed();
		}
		if (!e->down && held[e->scancode]) { held[e->scancode] = 0; pc_key(pc_scancode[e->scancode] | 0x80); }
		return;
	}
	uint8_t code = pc_scancode[e->scancode];
	if (!code) return;
	if (e->down) { /* repeats too, like a PC keyboard's typematic */
		held[e->scancode] = 1;
		pc_key(code);
	}
	else if (held[e->scancode]) {
		held[e->scancode] = 0;
		pc_key(code | 0x80);
	}
}

/* Release what is held when the window loses the keyboard, so no key sticks. */
static void release_all(void)
{
	for (int i = 0; i < SDL_SCANCODE_COUNT; i++) {
		if (held[i]) { held[i] = 0; pc_key(pc_scancode[i] | 0x80); }
	}
}

/* ---- the picture ---- */

static void present(void)
{
	static uint8_t pixels[320 * 200], palette[256 * 3];
	static uint64_t shown = ~0ull;
	uint64_t n = vga_frame(pixels, palette);
	if (n != shown) {
		shown = n;
		uint32_t colour[256];
		for (int i = 0; i < 256; i++) {
			const uint8_t *c = palette + 3 * i; /* 6 bits per component to 8 */
			colour[i] = 0xff000000u | (uint32_t)((c[0] << 2) | (c[0] >> 4)) << 16 |
				(uint32_t)((c[1] << 2) | (c[1] >> 4)) << 8 | (uint32_t)((c[2] << 2) | (c[2] >> 4));
		}
		void *dst;
		int pitch;
		if (SDL_LockTexture(picture, NULL, &dst, &pitch)) {
			for (int y = 0; y < 200; y++) {
				uint32_t *row = (uint32_t *)((uint8_t *)dst + y * pitch);
				for (int x = 0; x < 320; x++) row[x] = colour[pixels[y * 320 + x]];
			}
			SDL_UnlockTexture(picture);
		}
	}

	/* 320x200 shown at 4:3 (VGA pixels are 1.2 times as tall as wide), as large as fits */
	int w, h;
	SDL_GetRenderOutputSize(renderer, &w, &h);
	float dw = (float)w, dh = dw * 3 / 4;
	if (dh > (float)h) { dh = (float)h; dw = dh * 4 / 3; }
	SDL_FRect dst = { ((float)w - dw) / 2, ((float)h - dh) / 2, dw, dh };
	/* sharp but even pixels: enlarge by whole factors without smoothing, then scale smoothly */
	int sx = (int)SDL_ceilf(dw / 320), sy = (int)SDL_ceilf(dh / 200);
	if (sx < 1) sx = 1;
	if (sy < 1) sy = 1;
	if (!scaled || sx != scaled_x || sy != scaled_y) {
		if (scaled) SDL_DestroyTexture(scaled);
		scaled = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_TARGET, 320 * sx, 200 * sy);
		SDL_SetTextureScaleMode(scaled, SDL_SCALEMODE_LINEAR);
		scaled_x = sx;
		scaled_y = sy;
	}
	SDL_SetRenderTarget(renderer, scaled);
	SDL_RenderTexture(renderer, picture, NULL, NULL);
	SDL_SetRenderTarget(renderer, NULL);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	SDL_RenderTexture(renderer, scaled, NULL, &dst);
	SDL_RenderPresent(renderer);
	shown_at = dst;
}

/* ---- mouse: window position onto the 320x200 picture ---- */

/* In full screen the pointer is captured (relative mode), so it never reaches the screen edge
 * where macOS would show the menu bar or the Dock; the game gets the movement. In a window the
 * picture under the Mac's pointer is where the game's pointer goes. */
static void mouse(const SDL_Event *e)
{
	float wx, wy, rx, ry;
	SDL_MouseButtonFlags b = SDL_GetMouseState(&wx, &wy);
	int buttons = (b & SDL_BUTTON_LMASK ? 1 : 0) | (b & SDL_BUTTON_RMASK ? 2 : 0) | (b & SDL_BUTTON_MMASK ? 4 : 0);
	if (shown_at.w <= 0 || shown_at.h <= 0) return;
	if (SDL_GetWindowRelativeMouseMode(window)) {
		static float fx, fy; /* fractions of picture pixels not passed on yet */
		if (e->type == SDL_EVENT_MOUSE_MOTION) {
			float scale = SDL_GetWindowPixelDensity(window) * mouse_speed;
			fx += e->motion.xrel * scale * 320 / shown_at.w;
			fy += e->motion.yrel * scale * 200 / shown_at.h;
		}
		int dx = (int)fx, dy = (int)fy;
		fx -= (float)dx;
		fy -= (float)dy;
		pc_mouse_move(dx, dy, buttons);
		return;
	}
	SDL_RenderCoordinatesFromWindow(renderer, wx, wy, &rx, &ry);
	int x = (int)((rx - shown_at.x) * 320 / shown_at.w), y = (int)((ry - shown_at.y) * 200 / shown_at.h);
	x = x < 0 ? 0 : x > 319 ? 319 : x;
	y = y < 0 ? 0 : y > 199 ? 199 : y;
	pc_mouse(x, y, buttons);
}

/* ---- sound: the emulation's samples into an SDL audio stream (it converts the rate) ---- */

int host_audio_open(unsigned rate)
{
	if (!window || !SDL_InitSubSystem(SDL_INIT_AUDIO)) return 0;
	SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, (int)rate };
	audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
	if (!audio) {
		fprintf(stderr, "wc: no sound (%s)\n", SDL_GetError());
		return 0;
	}
	audio_rate = rate;
	static int16_t silence[2 * 4096];
	SDL_PutAudioStreamData(audio, silence, (int)(rate * 60 / 1000) * 4); /* 60 ms ahead against gaps */
	SDL_ResumeAudioStreamDevice(audio);
	return 1;
}

/* Called on the emulation's thread (SDL audio streams lock themselves). */
void host_audio(const int16_t *frames, unsigned count)
{
	if (!audio) return;
	if (SDL_GetAudioStreamQueued(audio) > (int)(audio_rate * 4 / 4)) { /* over 0.25 s behind: catch up */
		SDL_ClearAudioStream(audio);
	}
	SDL_PutAudioStreamData(audio, frames, (int)count * 4);
}

_Noreturn void host_exit(int code)
{
	exit(code);
}

/* Files the game writes are in the game folder already. */
void host_file_saved(const char *path)
{
	(void)path;
}

/* ---- threads ---- */

static void *game_thread(void *arg)
{
	((void (*)(void))arg)();
	host_exit(0);
}

_Noreturn void host_run(void (*game)(void))
{
	/* the recompiled code nests C calls as deeply as the game nests calls and interrupts */
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256u << 20);
	pthread_t thread;
	if (pthread_create(&thread, &attr, game_thread, (void *)game)) {
		fprintf(stderr, "wc: cannot start the game thread\n");
		exit(1);
	}
	if (!window) {
		pthread_join(thread, NULL);
		exit(0);
	}
	init_keys();
	show_speed();
	SDL_HideCursor(); /* the game draws its own */
	for (;;) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			switch (e.type) {
			case SDL_EVENT_QUIT: exit(0);
			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_KEY_UP: key(&e.key); break;
			case SDL_EVENT_WINDOW_FOCUS_LOST: release_all(); break;
			case SDL_EVENT_MOUSE_MOTION:
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP: mouse(&e); break;
			case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: SDL_SetWindowRelativeMouseMode(window, true); break;
			case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: SDL_SetWindowRelativeMouseMode(window, false); break;
			}
		}
		present(); /* waits for the display's refresh */
	}
}
