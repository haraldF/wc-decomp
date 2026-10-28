/* The mouse driver (INT 33h), fed from the host's window. Positions are in the driver's virtual
 * screen, which in mode 13h is 640x200 (x doubled), like Microsoft's driver. */
#include <stdatomic.h>
#include <stdio.h>

#include "host.h"
#include "wc_rt.h"

/* Events from the host, on its thread: a single-producer, single-consumer ring. */
static struct { int16_t x, y; uint8_t buttons, relative; } events[256];
static _Atomic uint32_t events_head, events_tail;
static _Atomic int mouse_seen; /* the host had a mouse event: the driver reports a mouse */

static void post(int x, int y, int buttons, int relative)
{
	uint32_t tail = atomic_load_explicit(&events_tail, memory_order_relaxed);
	if (tail - atomic_load_explicit(&events_head, memory_order_acquire) < 256) {
		events[tail % 256].x = (int16_t)x;
		events[tail % 256].y = (int16_t)y;
		events[tail % 256].buttons = (uint8_t)buttons;
		events[tail % 256].relative = (uint8_t)relative;
		atomic_store_explicit(&events_tail, tail + 1, memory_order_release);
	}
	atomic_store(&mouse_seen, 1);
}

void pc_warp_end(void);
void pc_mouse(int x, int y, int buttons) { if (buttons) pc_warp_end(); post(x, y, buttons, 0); }
void pc_mouse_move(int dx, int dy, int buttons) { if (buttons) pc_warp_end(); post(dx, dy, buttons, 1); }

static int x, y = 100, min_x, max_x = 639, min_y, max_y = 199;
static uint8_t buttons;     /* held now */
static uint8_t clicked;     /* pressed since the program last read the buttons (so short clicks count) */
/* A click shorter than this (a trackpad tap, a browser's synthetic click) is reported as held this
 * long: the barracks, for one, misses a button that is already up at its next poll. */
enum { MIN_CLICK_NS = 60000000 };
static uint64_t press_ns[3];
static uint8_t short_click; /* released before MIN_CLICK_NS: still reported as held */
uint64_t pc_now_ns(void);

static uint8_t reported_buttons(void)
{
	uint8_t b = buttons | clicked;
	if (short_click) {
		uint64_t now = pc_now_ns();
		for (int i = 0; i < 3; i++) {
			if (!(short_click & (1 << i))) continue;
			if (now - press_ns[i] < MIN_CLICK_NS) b |= (uint8_t)(1 << i);
			else short_click &= (uint8_t)~(1 << i);
		}
	}
	return b;
}
static uint16_t presses[3], releases[3];
static int16_t press_x[3], press_y[3], release_x[3], release_y[3];
static int mickey_x, mickey_y; /* motion counters since the last read (8 per 8 pixels) */
static int visible = -1;     /* cursor shown when >= 0; this driver draws none (the game draws its own) */

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Apply the host's events (called before every INT 33h service). */
static void drain(void)
{
	uint32_t head = atomic_load_explicit(&events_head, memory_order_relaxed);
	while (head != atomic_load_explicit(&events_tail, memory_order_acquire)) {
		int ex = events[head % 256].x, ey = events[head % 256].y;
		int nx = events[head % 256].relative ? x + ex * 2 : ex * 2, ny = events[head % 256].relative ? y + ey : ey;
		nx = clamp(nx, min_x, max_x);
		ny = clamp(ny, min_y, max_y);
		uint8_t nb = events[head % 256].buttons;
		head++;
		mickey_x += nx - x;
		mickey_y += (ny - y) * 2;
		x = nx;
		y = ny;
		for (int b = 0; b < 3; b++) {
			uint8_t bit = (uint8_t)(1 << b);
			if ((nb & bit) && !(buttons & bit)) {
				presses[b]++;
				press_ns[b] = pc_now_ns();
				short_click &= (uint8_t)~bit;
				press_x[b] = (int16_t)x;
				press_y[b] = (int16_t)y;
				clicked |= bit;
			}
			if (!(nb & bit) && (buttons & bit)) {
				releases[b]++;
				if (pc_now_ns() - press_ns[b] < MIN_CLICK_NS) short_click |= bit;
				release_x[b] = (int16_t)x;
				release_y[b] = (int16_t)y;
			}
		}
		buttons = nb;
	}
	atomic_store_explicit(&events_head, head, memory_order_release);
}

void mouse_int(uint32_t at)
{
	uint16_t ax = (uint16_t)rd(R_AX, 2), bx = (uint16_t)rd(R_BX, 2), cx = (uint16_t)rd(R_CX, 2),
		dx = (uint16_t)rd(R_DX, 2);
	if (wc_trace) {
		fprintf(stderr, "wc: INT 33h AX=%04x BX=%04x CX=%04x DX=%04x at %05x\n", ax, bx, cx, dx, at);
	}
	drain();
	switch (ax) {
	case 0x00: /* reset: installed with 2 buttons (if the host has a mouse) */
	case 0x21:
		if (!atomic_load(&mouse_seen) && !host_has_mouse()) {
			wr(R_AX, 2, 0);
			break;
		}
		min_x = 0; max_x = 639; min_y = 0; max_y = 199;
		x = clamp(x, min_x, max_x);
		y = clamp(y, min_y, max_y);
		visible = -1;
		wr(R_AX, 2, 0xffff);
		wr(R_BX, 2, 2);
		break;
	case 0x01: if (visible < 0) visible++; break; /* show cursor */
	case 0x02: visible--; break;                  /* hide cursor */
	case 0x03: /* position and buttons */
		wr(R_BX, 2, reported_buttons());
		clicked = 0;
		wr(R_CX, 2, (uint16_t)x);
		wr(R_DX, 2, (uint16_t)y);
		break;
	case 0x04: /* set position */
		x = clamp((int16_t)cx, min_x, max_x);
		y = clamp((int16_t)dx, min_y, max_y);
		break;
	case 0x05: /* button press information */
	case 0x06: { /* button release information */
		int b = bx < 3 ? bx : 0;
		wr(R_AX, 2, reported_buttons());
		if (ax == 5) {
			wr(R_BX, 2, presses[b]);
			wr(R_CX, 2, (uint16_t)press_x[b]);
			wr(R_DX, 2, (uint16_t)press_y[b]);
			presses[b] = 0;
		}
		else {
			wr(R_BX, 2, releases[b]);
			wr(R_CX, 2, (uint16_t)release_x[b]);
			wr(R_DX, 2, (uint16_t)release_y[b]);
			releases[b] = 0;
		}
		break;
	}
	case 0x07: /* horizontal range */
		min_x = (int16_t)(cx < dx ? cx : dx);
		max_x = (int16_t)(cx < dx ? dx : cx);
		x = clamp(x, min_x, max_x);
		break;
	case 0x08: /* vertical range */
		min_y = (int16_t)(cx < dx ? cx : dx);
		max_y = (int16_t)(cx < dx ? dx : cx);
		y = clamp(y, min_y, max_y);
		break;
	case 0x0b: /* motion counters */
		wr(R_CX, 2, (uint16_t)mickey_x);
		wr(R_DX, 2, (uint16_t)mickey_y);
		mickey_x = mickey_y = 0;
		break;
	case 0x24: /* driver version 8.0, PS/2 mouse */
		wr(R_BX, 2, 0x0800);
		wr(R_CX, 2, 0x0400);
		break;
	case 0x0a: case 0x0f: case 0x10: case 0x13: case 0x1a: case 0x1d:
		break; /* cursor shape, mickey ratio, exclusion area, speed, sensitivity, page: nothing to do */
	default:
		fprintf(stderr, "wc: unsupported mouse function INT 33h AX=%04x at %05x (ignored)\n", ax, at);
		break;
	}
}
