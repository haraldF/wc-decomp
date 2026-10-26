/* VGA: the DAC (palette), the status register's blanking and retrace bits in real time, and the
 * frames the host shows, copied from the mode 13h frame buffer at each vertical retrace. */
#include <pthread.h>
#include <stdio.h>

#include "host.h"
#include "wc_rt.h"

uint64_t pc_now_ns(void);

/* ---- DAC: index at 3C8h (write) / 3C7h (read), then R, G, B (6 bits) at 3C9h ---- */

uint8_t wc_palette[256][3];
static uint8_t dac_write, dac_read, dac_part;

/* ---- timing: mode 13h is 400 lines (200 doubled) of 449, at 31.78 us a line: 70.09 Hz ---- */

#define LINE_NS 31778u
#define LINES 449u
#define FRAME_NS ((uint64_t)LINE_NS * LINES)
#define RETRACE_LINE 412u /* vertical retrace: 2 lines, inside the blanking from line 400 */

static uint64_t start_ns;
static uint64_t retraces_seen; /* retraces the program has seen in the status register */

/* The program runs in bursts (the CPU speed limit sleeps in between), so it can miss the 64 us
 * retrace while polling: a retrace that began since the last read is reported once, so that a
 * "wait for retrace" loop still sees every frame. */
static uint8_t status(void)
{
	uint64_t since = pc_now_ns() - start_ns, r = (uint64_t)RETRACE_LINE * LINE_NS;
	uint64_t t = since % FRAME_NS;
	uint64_t retraces = since >= r ? (since - r) / FRAME_NS + 1 : 0; /* retraces begun so far */
	uint32_t line = (uint32_t)(t / LINE_NS), dot = (uint32_t)(t % LINE_NS);
	uint8_t v = 0;
	if (line >= 400 || dot >= LINE_NS * 4 / 5) v |= 0x01; /* blanking (display disabled) */
	if (line >= RETRACE_LINE && line < RETRACE_LINE + 2) {
		v |= 0x08;
		retraces_seen = retraces;
	}
	else if (retraces > retraces_seen) {
		v |= 0x09;
		retraces_seen = retraces;
	}
	return v;
}

/* ---- frames for the host ---- */

static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t frame_pixels[320 * 200], frame_palette[256 * 3];
static uint64_t frame_count, frame_taken;

/* Called regularly on the emulation's thread: at each retrace, copy the picture for the host. */
void vga_poll(uint64_t now)
{
	uint64_t frame = (now - start_ns + FRAME_NS - (uint64_t)RETRACE_LINE * LINE_NS) / FRAME_NS;
	if (frame == frame_taken) return;
	frame_taken = frame;
	pthread_mutex_lock(&frame_lock);
	memcpy(frame_pixels, M + 0xa0000, sizeof frame_pixels);
	memcpy(frame_palette, wc_palette, sizeof frame_palette);
	frame_count++;
	pthread_mutex_unlock(&frame_lock);
}

uint64_t vga_frame(uint8_t pixels[320 * 200], uint8_t palette[256 * 3])
{
	pthread_mutex_lock(&frame_lock);
	memcpy(pixels, frame_pixels, sizeof frame_pixels);
	memcpy(palette, frame_palette, sizeof frame_palette);
	uint64_t n = frame_count;
	pthread_mutex_unlock(&frame_lock);
	return n;
}

/* ---- ports ---- */

uint64_t vga_in(uint64_t port)
{
	uint64_t v = 0;
	switch (port) {
	case 0x3da: v = status(); break;
	case 0x3c9:
		v = wc_palette[dac_read][dac_part];
		if (++dac_part == 3) { dac_part = 0; dac_read++; }
		break;
	}
	return v;
}

void vga_out(uint64_t port, uint64_t value)
{
	switch (port) {
	case 0x3c7: dac_read = (uint8_t)value; dac_part = 0; break;
	case 0x3c8: dac_write = (uint8_t)value; dac_part = 0; break;
	case 0x3c9:
		wc_palette[dac_write][dac_part] = (uint8_t)(value & 0x3f);
		if (++dac_part == 3) { dac_part = 0; dac_write++; }
		break;
	}
}

void vga_init(void)
{
	start_ns = pc_now_ns();
}

/* ---- video BIOS (INT 10h): modes and the DAC; text output is not shown ---- */

void vga_bios(uint32_t at)
{
	uint8_t ah = (uint8_t)rd(R_AH, 1), al = (uint8_t)rd(R_AL, 1);
	uint16_t bx = (uint16_t)rd(R_BX, 2), cx = (uint16_t)rd(R_CX, 2), dx = (uint16_t)rd(R_DX, 2);
	uint32_t table = (uint32_t)lin(rd(R_ES, 2), dx);
	switch (ah) {
	case 0x00: { /* set mode */
		uint8_t mode = al & 0x7f;
		M[0x449] = mode;
		mwr(0x44a, 2, mode == 0x13 || mode <= 1 ? 40 : 80);
		if (mode == 0x13 && !(al & 0x80)) memset(M + 0xa0000, 0, 0x10000);
		/* the first 16 DAC entries: the default EGA colours */
		static const uint8_t ega[16][3] = {
			{ 0, 0, 0 }, { 0, 0, 42 }, { 0, 42, 0 }, { 0, 42, 42 }, { 42, 0, 0 }, { 42, 0, 42 }, { 42, 21, 0 },
			{ 42, 42, 42 }, { 21, 21, 21 }, { 21, 21, 63 }, { 21, 63, 21 }, { 21, 63, 63 }, { 63, 21, 21 },
			{ 63, 21, 63 }, { 63, 63, 21 }, { 63, 63, 63 },
		};
		memset(wc_palette, 0, sizeof wc_palette);
		memcpy(wc_palette, ega, sizeof ega);
		break;
	}
	case 0x0f: /* get mode */
		wr(R_AL, 1, M[0x449]);
		wr(R_AH, 1, M[0x44a]);
		wr(R_BH, 1, 0);
		break;
	case 0x10: /* palette */
		switch (al) {
		case 0x10: /* one DAC register: BX, DH = red, CH = green, CL = blue */
			wc_palette[bx & 0xff][0] = (uint8_t)(dx >> 8) & 0x3f;
			wc_palette[bx & 0xff][1] = (uint8_t)(cx >> 8) & 0x3f;
			wc_palette[bx & 0xff][2] = (uint8_t)cx & 0x3f;
			break;
		case 0x12: /* CX registers from BX, from the table at ES:DX */
			for (uint32_t i = 0; i < cx && bx + i < 256; i++) {
				for (int c = 0; c < 3; c++) wc_palette[bx + i][c] = M[(table + 3 * i + c) & 0xfffff] & 0x3f;
			}
			break;
		case 0x15: /* read one */
			wr(R_DH, 1, wc_palette[bx & 0xff][0]);
			wr(R_CH, 1, wc_palette[bx & 0xff][1]);
			wr(R_CL, 1, wc_palette[bx & 0xff][2]);
			break;
		case 0x17: /* read CX registers from BX into ES:DX */
			for (uint32_t i = 0; i < cx && bx + i < 256; i++) {
				for (int c = 0; c < 3; c++) mwr(table + 3 * i + c, 1, wc_palette[bx + i][c]);
			}
			break;
		}
		break;
	case 0x12:
		if ((bx & 0xff) == 0x10) { /* EGA/VGA information: colour, 256 KB */
			wr(R_BX, 2, 0x0003);
			wr(R_CX, 2, 0x0009);
		}
		break;
	case 0x1a:
		if (al == 0) { /* display combination: VGA with colour display */
			wr(R_AL, 1, 0x1a);
			wr(R_BX, 2, 0x0008);
		}
		break;
	}
	if (wc_trace) fprintf(stderr, "wc: INT 10h AX=%02x%02x BX=%04x CX=%04x DX=%04x at %05x\n", ah, al, bx, cx, dx, at);
}
