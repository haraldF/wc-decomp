/* PC hardware around the CPU: interrupt delivery, the 8259 interrupt controller, the 8253 timer,
 * the keyboard controller and the ROM BIOS entry points (every interrupt vector starts out
 * pointing to F000:nn, handled here natively). */
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "host.h"
#include "wc_rt.h"

void dos_int(uint64_t n, uint32_t at);
uint64_t vga_in(uint64_t port);
uint64_t opl_in(uint64_t port);
void opl_out(uint64_t port, uint64_t value);
void opl_poll(uint64_t now);
void opl_init(void);
void ems_init(void);
void vga_out(uint64_t port, uint64_t value);
void vga_poll(uint64_t now);
void vga_init(void);

int wc_irq_pending;               /* IRQs raised but not yet delivered (bit per IRQ) */
static uint8_t pic_mask, pic_in_service;
static uint64_t irq_count[8];

void pc_report(FILE *f)
{
	fprintf(f, "  interrupts: timer %llu, keyboard %llu; pending %02x, in service %02x, mask %02x, IF=%llu\n",
		(unsigned long long)irq_count[0], (unsigned long long)irq_count[1], wc_irq_pending, pic_in_service,
		pic_mask, rd(R_IF, 1));
}

#define BIOS_SEG 0xf000u

/* ---- flags and the stack ---- */

static const struct { uint32_t reg; int bit; } flag_bits[] = {
	{ R_CF, 0 }, { R_PF, 2 }, { R_AF, 4 }, { R_ZF, 6 }, { R_SF, 7 },
	{ R_TF, 8 }, { R_IF, 9 }, { R_DF, 10 }, { R_OF, 11 },
};

static uint16_t get_flags(void)
{
	uint16_t f = 0x0002;
	for (size_t i = 0; i < sizeof flag_bits / sizeof *flag_bits; i++) {
		f |= (rd(flag_bits[i].reg, 1) ? 1 : 0) << flag_bits[i].bit;
	}
	return f;
}

void set_flags_word(uint16_t f)
{
	for (size_t i = 0; i < sizeof flag_bits / sizeof *flag_bits; i++) {
		wr(flag_bits[i].reg, 1, (f >> flag_bits[i].bit) & 1);
	}
}

static void push16(uint16_t v)
{
	wr(R_SP, 2, rd(R_SP, 2) - 2);
	mwr(lin(rd(R_SS, 2), rd(R_SP, 2)), 2, v);
}

static uint16_t pop16(void)
{
	uint16_t v = (uint16_t)mrd(lin(rd(R_SS, 2), rd(R_SP, 2)), 2);
	wr(R_SP, 2, rd(R_SP, 2) + 2);
	return v;
}

/* Interrupt through the vector table, returning to ret (a runtime linear address in CS). */
static void interrupt(uint8_t vec, uint32_t ret, uint32_t ret_code)
{
	uint32_t cs = (uint32_t)rd(R_CS, 2);
	push16(get_flags());
	push16((uint16_t)cs);
	push16((uint16_t)(ret - cs * 16));
	wr(R_IF, 1, 0);
	wr(R_TF, 1, 0);
	uint16_t off = (uint16_t)mrd(vec * 4, 2), seg = (uint16_t)mrd(vec * 4 + 2, 2);
	uint16_t sp = (uint16_t)rd(R_SP, 2);
	if (wc_trace && vec >= 8 && vec < 0x10) {
		fprintf(stderr, "wc: IRQ%d -> %04x:%04x (interrupted %05x, SP=%04x)\n", vec - 8, seg, off, ret_code, sp);
	}
	wr(R_CS, 2, seg);
	push_ret(ret_code);
	run((uint32_t)lin(seg, off));
	pop_ret();
	if (wc_trace && vec >= 8 && vec < 0x10) {
		fprintf(stderr, "wc: IRQ%d returned: IF=%llu SP=%04llx (expected %04x) CS=%04llx\n", vec - 8, rd(R_IF, 1),
			rd(R_SP, 2), (uint16_t)(sp), rd(R_CS, 2));
	}
}

/* ---- 8253 timer, channel 0 ---- */

/* Quick start: until the first picture (the intro's first frame), the game mostly waits (it shows
 * its loading messages for a while), so emulated time runs WARP times as fast and instructions
 * aren't limited. It ends at the first visible frame or the first key or mouse button (WC_WARP=0:
 * off). The clock stays continuous: emulated time = real time + WARP - 1 times the real time spent
 * warping. */
enum { WARP = 20 };
static uint64_t warp_start_ns;
static _Atomic uint64_t warp_end_ns = 1; /* 0: warping; set when it ends (1: never started) */

static uint64_t real_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

uint64_t pc_now_ns(void)
{
	uint64_t now = real_ns(), end = atomic_load_explicit(&warp_end_ns, memory_order_acquire);
	if (end == 1) return now;
	return now + ((end ? end : now) - warp_start_ns) * (WARP - 1);
}

int pc_warping(void)
{
	return atomic_load_explicit(&warp_end_ns, memory_order_relaxed) == 0;
}

void pc_warp_end(void)
{
	uint64_t zero = 0;
	atomic_compare_exchange_strong(&warp_end_ns, &zero, real_ns());
}

static void warp_start(void)
{
	const char *w = getenv("WC_WARP");
	if (w && !strcmp(w, "0")) return;
	warp_start_ns = real_ns();
	atomic_store(&warp_end_ns, 0);
}

static uint32_t pit_divisor = 65536; /* 18.2 Hz */
static uint8_t pit_access = 3, pit_mode = 3, pit_low_next = 1, pit_low;
static uint64_t pit_next_ns, pit_period_ns, pit_start_ns;
static uint16_t pit_latch;
static uint8_t pit_latched, pit_read_low_next = 1;

static void pit_set(uint32_t divisor)
{
	pit_divisor = divisor ? divisor : 65536;
	pit_period_ns = (uint64_t)pit_divisor * 1000000000u / 1193182u;
	pit_start_ns = pc_now_ns();
	pit_next_ns = pit_start_ns + pit_period_ns;
	if (wc_trace) {
		fprintf(stderr, "wc: timer at %.1f Hz\n", 1193182.0 / pit_divisor);
	}
}

/* The counter as the game reads it: counting down at 1.193182 MHz from the divisor; in mode 3
 * (square wave) by 2 per clock, twice per period. */
static uint16_t pit_count(void)
{
	uint64_t clocks = (pc_now_ns() - pit_start_ns) * 1193182u / 1000000000u;
	if (pit_mode == 3) {
		return (uint16_t)((pit_divisor - (clocks * 2) % pit_divisor) & 0xfffe);
	}
	return (uint16_t)(pit_divisor - clocks % pit_divisor);
}

/* ---- keyboard ---- */

static uint8_t kbd_queue[64], kbd_head, kbd_tail, kbd_data;
static int kbd_tty;
static struct termios kbd_saved;

static void kbd_push(uint8_t scancode)
{
	uint8_t next = (uint8_t)((kbd_tail + 1) % sizeof kbd_queue);
	if (next != kbd_head) {
		kbd_queue[kbd_tail] = scancode;
		kbd_tail = next;
	}
}

static const struct { const char *name; char ch; uint8_t code; } keys[] = {
	{ "esc", 27, 0x01 }, { "enter", '\r', 0x1c }, { "space", ' ', 0x39 }, { "tab", '\t', 0x0f },
	{ "backspace", 127, 0x0e }, { "up", 0, 0x48 }, { "down", 0, 0x50 }, { "left", 0, 0x4b },
	{ "right", 0, 0x4d }, { "f1", 0, 0x3b }, { "f2", 0, 0x3c }, { "f3", 0, 0x3d }, { "f10", 0, 0x44 },
};
static const char row_chars[] = "1234567890-=\0\0qwertyuiop[]\0\0asdfghjkl;'`\0\\zxcvbnm,./";

static uint8_t scancode_for(char c)
{
	for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
		if (keys[i].ch && keys[i].ch == c) return keys[i].code;
	}
	if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
	for (size_t i = 0; i < sizeof row_chars - 1; i++) {
		if (row_chars[i] && row_chars[i] == c) return (uint8_t)(0x02 + i);
	}
	if (c == '\n') return 0x1c;
	return 0;
}

static void press(uint8_t code)
{
	if (code) {
		kbd_push(code);
		kbd_push(code | 0x80);
	}
}

/* WC_KEYS="tick:key,...": keys to press after that many timer interrupts, for testing.
 * A key is a character or a name (esc, enter, space, up, ...); it is pressed and released at
 * once, or only pressed with a + in front (+up) and only released with a - (-up). The mouse:
 * @x.y moves it (320x200), lmb/rmb click (+lmb/-lmb hold and release); with WC_MOUSE=1. */
enum { SCRIPT_MAX = 1024 };
static struct { uint64_t tick; uint8_t code; char how; int16_t mx, my; } script[SCRIPT_MAX];
static int script_mx = 160, script_my = 100, script_buttons;
static int script_len, script_pos;
static uint64_t ticks;
static uint8_t script_held;          /* key pressed with + and not released yet */
static uint64_t script_repeat_ns;    /* when it repeats next (typematic: 0.5 s, then 10.9 per second) */

static void parse_script(const char *s)
{
	while (s && *s) {
		if (script_len == SCRIPT_MAX) {
			fprintf(stderr, "wc: WC_KEYS has more than %d entries\n", SCRIPT_MAX);
			exit(1);
		}
		unsigned long long t;
		char name[16];
		int used = 0;
		if (sscanf(s, "%llu:%15[^,]%n", &t, name, &used) != 2) {
			fprintf(stderr, "wc: bad WC_KEYS at \"%s\"\n", s);
			exit(1);
		}
		char how = 0, *n = name;
		if ((*n == '+' || *n == '-') && n[1]) how = *n++;
		int mx, my;
		if (sscanf(n, "@%d.%d", &mx, &my) == 2) { /* mouse to x.y */
			script[script_len].tick = t;
			script[script_len].how = 'm';
			script[script_len].mx = (int16_t)mx;
			script[script_len++].my = (int16_t)my;
			s += used + (s[used] == ',');
			continue;
		}
		if (!strcmp(n, "lmb") || !strcmp(n, "rmb")) { /* mouse button: code 1 left, 2 right */
			script[script_len].tick = t;
			script[script_len].how = how ? how : 'c';
			script[script_len].mx = -1;
			script[script_len++].code = n[0] == 'l' ? 1 : 2;
			s += used + (s[used] == ',');
			continue;
		}
		uint8_t code = 0;
		for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
			if (!strcmp(n, keys[i].name)) code = keys[i].code;
		}
		if (!code && strlen(n) == 1) code = scancode_for(n[0]);
		if (!code) {
			fprintf(stderr, "wc: unknown key \"%s\" in WC_KEYS\n", name);
			exit(1);
		}
		script[script_len].tick = t;
		script[script_len].how = how;
		script[script_len++].code = code;
		s += used + (s[used] == ',');
	}
}

/* Keys from the host's window, on its thread: a single-producer, single-consumer ring. */
static uint8_t host_keys[256];
static _Atomic uint32_t host_keys_head, host_keys_tail;
/* The game samples keys in its menus, so a key released this soon after it went down (a quick tap,
 * a browser's synthetic key press) has its release held back until then. */
enum { MIN_KEY_NS = 60000000 };
static uint64_t key_down_ns[128];

void pc_key(uint8_t scancode)
{
	if (!(scancode & 0x80)) pc_warp_end();
	uint32_t tail = atomic_load_explicit(&host_keys_tail, memory_order_relaxed);
	if (tail - atomic_load_explicit(&host_keys_head, memory_order_acquire) < sizeof host_keys) {
		host_keys[tail % sizeof host_keys] = scancode;
		atomic_store_explicit(&host_keys_tail, tail + 1, memory_order_release);
	}
}

static void restore_tty(void)
{
	tcsetattr(0, TCSANOW, &kbd_saved);
}

static void poll_keyboard(void)
{
	while (script_pos < script_len && ticks >= script[script_pos].tick) {
		uint8_t code = script[script_pos].code;
		char how = script[script_pos].how;
		int16_t mx = script[script_pos].mx, my = script[script_pos++].my;
		if (how == 'm') {
			script_mx = mx;
			script_my = my;
			pc_mouse(script_mx, script_my, script_buttons);
		}
		else if (mx == -1) { /* mouse button: click, or + press / - release */
			if (how != '-') pc_mouse(script_mx, script_my, script_buttons |= code);
			if (how != '+') pc_mouse(script_mx, script_my, script_buttons &= ~code);
		}
		else if (how == '+') {
			kbd_push(code);
			script_held = code;
			script_repeat_ns = pc_now_ns() + 500000000u;
		}
		else if (how == '-') {
			kbd_push(code | 0x80);
			if (script_held == code) script_held = 0;
		}
		else press(code);
	}
	if (script_held && pc_now_ns() >= script_repeat_ns) {
		kbd_push(script_held);
		script_repeat_ns += 92000000u;
	}
	uint32_t head = atomic_load_explicit(&host_keys_head, memory_order_relaxed);
	while (head != atomic_load_explicit(&host_keys_tail, memory_order_acquire)) {
		uint8_t code = host_keys[head % sizeof host_keys];
		uint64_t now = pc_now_ns();
		if (!(code & 0x80)) key_down_ns[code] = now;
		else if (now - key_down_ns[code & 0x7f] < MIN_KEY_NS) break; /* later keys wait behind it */
		kbd_push(code);
		head++;
	}
	atomic_store_explicit(&host_keys_head, head, memory_order_release);
	if (kbd_tty) {
		char c;
		while (read(0, &c, 1) == 1) {
			press(scancode_for(c));
		}
	}
	if (kbd_head != kbd_tail) {
		wc_irq_pending |= 2;
	}
}

/* ---- interrupt controller and delivery ---- */

static void poll_devices(void)
{
	{
		uint64_t now = pc_now_ns();
		if (pit_period_ns && now >= pit_next_ns) {
			wc_irq_pending |= 1;
			pit_next_ns += pit_period_ns;
			if (now > pit_next_ns + 50 * pit_period_ns) {
				pit_next_ns = now + pit_period_ns; /* don't replay a long backlog */
			}
		}
		poll_keyboard();
		vga_poll(now);
		opl_poll(now);
	}
}

static void deliver(uint32_t at);

/* CPU speed: Wing Commander paces animation and space flight by CPU speed, so run at most
 * cycles_per_ms instructions per millisecond of real time, like DOSBox's "cycles". WC_CYCLES sets
 * it ("max": as fast as possible); the host changes it while running (pc_cycles).
 * In flight the game draws more or less per frame (enemies close by: more), so a fixed
 * instruction rate gives an uneven frame rate. There it runs at full speed and pc_frame() paces
 * the frames instead (flight_fps, WC_FPS; 0: no frame pacing). */
static _Atomic uint64_t cycles_per_ms = 8334;
static _Atomic uint64_t flight_fps = 15;
static uint64_t throttle_start_ns, throttle_start_icount, throttle_cycles;
/* Frames are paced while the game copies frames to the screen; between two copies it may use up
 * to 3 frames' worth of instructions at full speed (heavy frames need it). Code that goes on
 * without copying frames (the autopilot sequence) is limited by instructions again at once. */
static int pacing;
static _Atomic int pacing_seen; /* pacing, for the host's thread (pc_in_flight) */
static uint64_t next_frame_ns, frame_icount;

static uint64_t faster(uint64_t c, int change, uint64_t lo, uint64_t hi)
{
	if (!change || !c) return c;
	c = change > 0 ? c + c / 5 : c - c / 6;
	return c < lo ? lo : c > hi ? hi : c;
}

uint64_t pc_cycles(int change)
{
	uint64_t c = faster(atomic_load(&cycles_per_ms), change, 500, 1000000);
	atomic_store(&cycles_per_ms, c);
	return c;
}

uint64_t pc_flight_fps(int change)
{
	uint64_t f = faster(atomic_load(&flight_fps), change, 5, 200);
	atomic_store(&flight_fps, f);
	return f;
}

int pc_in_flight(void)
{
	return atomic_load_explicit(&pacing_seen, memory_order_relaxed);
}

static void throttle(void)
{
	uint64_t cycles = atomic_load_explicit(&cycles_per_ms, memory_order_relaxed);
	if (!cycles) return;
	if (pc_warping()) {
		throttle_cycles = 0; /* measure afresh once it ends */
		return;
	}
	uint64_t now = pc_now_ns();
	if (pacing) {
		uint64_t fps = atomic_load_explicit(&flight_fps, memory_order_relaxed);
		if (fps && wc_icount - frame_icount < 3 * cycles * 1000 / fps) { /* within the frame budget */
			throttle_start_ns = now;
			throttle_start_icount = wc_icount;
			return;
		}
		pacing = 0; /* no frame copy for a while: limit instructions from here */
		atomic_store_explicit(&pacing_seen, 0, memory_order_relaxed);
		throttle_start_ns = now;
		throttle_start_icount = wc_icount;
	}
	if (cycles != throttle_cycles) { /* changed: measure from here */
		throttle_cycles = cycles;
		throttle_start_ns = now;
		throttle_start_icount = wc_icount;
	}
	uint64_t due_ns = throttle_start_ns + (wc_icount - throttle_start_icount) * 1000000u / cycles;
	if (due_ns > now + 1000000u) { /* more than 1 ms ahead: wait */
		struct timespec ts = { 0, (long)(due_ns - now) };
		nanosleep(&ts, NULL);
	}
	else if (now > due_ns + 50000000u) { /* far behind (e.g. waiting for a key): don't catch up */
		throttle_start_ns = now;
		throttle_start_icount = wc_icount;
	}
}

void irq_poll(uint32_t at)
{
	if (!(wc_icount & 0x3ff)) {
		throttle();
		poll_devices();
	}
	deliver(at);
}

/* A frame is done (in flight): wait for its slot, keeping devices and interrupts going. */
void pc_frame(uint32_t at)
{
	uint64_t fps = atomic_load_explicit(&flight_fps, memory_order_relaxed);
	if (!fps) return;
	uint64_t now = pc_now_ns(), period = 1000000000u / fps;
	static int stats = -1;
	if (stats < 0) stats = getenv("WC_STATS") != NULL;
	if (stats) { /* WC_STATS: frames per second and the longest frame, every second */
		static uint64_t second_ns, frames, longest, last;
		if (last && now - last > longest) longest = now - last;
		last = now;
		frames++;
		if (now - second_ns >= 1000000000u) {
			fprintf(stderr, "wc: flight: %llu frames/s, longest %llu ms\n", (unsigned long long)frames,
				(unsigned long long)(longest / 1000000));
			second_ns = now;
			frames = longest = 0;
		}
	}
	if (!pacing || now > next_frame_ns + 4 * period) next_frame_ns = now; /* (re)start */
	pacing = 1;
	atomic_store_explicit(&pacing_seen, 1, memory_order_relaxed);
	next_frame_ns += period;
	while ((now = pc_now_ns()) < next_frame_ns) {
		uint64_t wait = next_frame_ns - now;
		struct timespec ts = { 0, (long)(wait < 1000000u ? wait : 1000000u) };
		nanosleep(&ts, NULL);
		poll_devices();
		deliver(at);
	}
	frame_icount = wc_icount;
}

/* A BIOS or DOS service waiting (for a key): let time pass with interrupts enabled. */
static void pc_wait(uint32_t at)
{
	uint64_t saved_if = rd(R_IF, 1);
	wr(R_IF, 1, 1);
	usleep(1000);
	poll_devices();
	deliver(at);
	wr(R_IF, 1, saved_if);
}

static void deliver(uint32_t at)
{
	if (!rd(R_IF, 1)) {
		return;
	}
	int ready = wc_irq_pending & ~pic_mask & ~pic_in_service;
	for (int irq = 0; irq < 8; irq++) {
		if (ready & (1 << irq)) {
			/* nothing of lower number may be in service either */
			if (pic_in_service & ((1 << irq) - 1)) return;
			wc_irq_pending &= ~(1 << irq);
			pic_in_service |= (uint8_t)(1 << irq);
			irq_count[irq]++;
			if (irq == 0) ticks++;
			if (irq == 1 && kbd_head != kbd_tail) {
				kbd_data = kbd_queue[kbd_head];
				kbd_head = (uint8_t)((kbd_head + 1) % sizeof kbd_queue);
			}
			interrupt((uint8_t)(8 + irq), runtime_addr(at), at);
			return;
		}
	}
}

static void eoi(void)
{
	for (int irq = 0; irq < 8; irq++) {
		if (pic_in_service & (1 << irq)) {
			pic_in_service &= (uint8_t)~(1 << irq);
			return;
		}
	}
}

uint64_t port_in(uint64_t port, int size)
{
	uint64_t v = 0;
	switch (port) {
	case 0x20: v = pic_in_service; break;
	case 0x21: v = pic_mask; break;
	case 0x40: { /* timer channel 0 counter: latched or live, low byte then high byte */
		uint16_t c = pit_latched ? pit_latch : pit_count();
		if (pit_access == 2 || (pit_access == 3 && !pit_read_low_next)) {
			v = c >> 8;
			pit_read_low_next = 1;
			pit_latched = 0;
		}
		else {
			v = c & 0xff;
			if (pit_access == 3) pit_read_low_next = 0;
			else pit_latched = 0;
		}
		break;
	}
	case 0x60: v = kbd_data; break;
	case 0x61: v = 0; break;
	case 0x201: v = 0xff; break; /* game port: no joystick (axes never time out, buttons up) */
	case 0x64: v = kbd_head != kbd_tail ? 1 : 0; break;
	case 0x3c7: case 0x3c8: case 0x3c9: case 0x3da: v = vga_in(port); break;
	case 0x388: case 0x389: v = opl_in(port); break;
	}
	if (wc_trace >= 3) fprintf(stderr, "wc: in  port %03llx -> %02llx\n", port, v);
	(void)size;
	return v;
}

void port_out(uint64_t port, uint64_t value, int size)
{
	if (wc_trace >= 3) fprintf(stderr, "wc: out port %03llx <- %0*llx\n", port, size * 2, value);
	switch (port) {
	case 0x20:
		if ((value & 0xe0) == 0x20) eoi(); /* non-specific or specific EOI */
		break;
	case 0x21:
		pic_mask = (uint8_t)value;
		break;
	case 0x3c7: case 0x3c8: case 0x3c9: vga_out(port, value); break;
	case 0x388: case 0x389: opl_out(port, value); break;
	case 0x43:
		if ((value >> 6) == 0) { /* channel 0 */
			if (((value >> 4) & 3) == 0) { /* counter latch */
				pit_latch = pit_count();
				pit_latched = 1;
				pit_read_low_next = 1;
			}
			else {
				pit_access = (value >> 4) & 3;
				pit_mode = (value >> 1) & 7;
				pit_low_next = 1;
				pit_read_low_next = 1;
			}
		}
		break;
	case 0x40:
		if (pit_access == 3) { /* low byte, then high byte */
			if (pit_low_next) {
				pit_low = (uint8_t)value;
				pit_low_next = 0;
			}
			else {
				pit_set(pit_low | (uint32_t)(value & 0xff) << 8);
				pit_low_next = 1;
			}
		}
		else if (pit_access == 1) {
			pit_set((uint32_t)(value & 0xff));
		}
		else if (pit_access == 2) {
			pit_set((uint32_t)(value & 0xff) << 8);
		}
		break;
	}
}

/* ---- BIOS keyboard: INT 9 turns scancodes into keys (scancode << 8 | character) in the buffer
 * at 0040:001E..003D, as a real BIOS does; INT 16h and DOS's console input read them ---- */

enum { KBUF_HEAD = 0x41a, KBUF_TAIL = 0x41c, KBUF_START = 0x1e, KBUF_END = 0x3e, SHIFT_FLAGS = 0x417 };

/* characters for scancodes 00..39, without and with Shift */
static const char key_lower[] = "\0\x1b" "1234567890-=" "\b\t" "qwertyuiop[]" "\r\0" "asdfghjkl;'`" "\0\\" "zxcvbnm,./" "\0*\0 ";
static const char key_upper[] = "\0\x1b" "!@#$%^&*()_+" "\b\t" "QWERTYUIOP{}" "\r\0" "ASDFGHJKL:\"~" "\0|" "ZXCVBNM<>?" "\0*\0 ";

static void bios_keyboard(void)
{
	uint8_t code = kbd_data & 0x7f, up = kbd_data & 0x80, flags = M[SHIFT_FLAGS];
	uint8_t bit = code == 0x36 ? 0x01 : code == 0x2a ? 0x02 : code == 0x1d ? 0x04 : code == 0x38 ? 0x08 : 0;
	if (bit) {
		M[SHIFT_FLAGS] = up ? flags & ~bit : flags | bit;
		return;
	}
	if (up) return;
	uint8_t ch = 0;
	if (code < sizeof key_lower - 1 && !(flags & 0x08)) {
		ch = (uint8_t)((flags & 0x03) ? key_upper[code] : key_lower[code]);
		if ((flags & 0x04) && ch >= 'a' && ch <= 'z') ch &= 0x1f;
	}
	uint16_t tail = (uint16_t)mrd(KBUF_TAIL, 2), next = tail + 2 == KBUF_END ? KBUF_START : tail + 2;
	if (next != mrd(KBUF_HEAD, 2)) {
		mwr(0x400 + tail, 2, (uint64_t)code << 8 | ch);
		mwr(KBUF_TAIL, 2, next);
	}
}

/* The next key in the BIOS buffer, if any (left in the buffer). */
int bios_key_peek(uint16_t *key)
{
	uint16_t head = (uint16_t)mrd(KBUF_HEAD, 2);
	if (head == mrd(KBUF_TAIL, 2)) return 0;
	*key = (uint16_t)mrd(0x400 + head, 2);
	return 1;
}

/* The next key, waiting for one. */
uint16_t bios_key_get(uint32_t at)
{
	uint16_t key;
	while (!bios_key_peek(&key)) pc_wait(at);
	uint16_t head = (uint16_t)mrd(KBUF_HEAD, 2) + 2;
	mwr(KBUF_HEAD, 2, head == KBUF_END ? KBUF_START : head);
	return key;
}

void bios_key_flush(void)
{
	mwr(KBUF_HEAD, 2, mrd(KBUF_TAIL, 2));
}

/* ---- software interrupts and the ROM BIOS ---- */

/* INT n in the program: through the vector table, so the program's own handlers run. */
void cpu_int(uint64_t n, uint32_t at, uint32_t next)
{
	uint16_t seg = (uint16_t)mrd(n * 4 + 2, 2), off = (uint16_t)mrd(n * 4, 2);
	if (seg == BIOS_SEG && off == n) {
		dos_int(n, at); /* still the BIOS/DOS: no need to go through the stack */
		return;
	}
	interrupt((uint8_t)n, runtime_addr(next), next);
}

/* Control reached F000:nn, the BIOS entry for vector nn (an INT, or a chained handler doing
 * PUSHF / CALL FAR or JMP FAR to the old vector): handle it and return like IRET. */
static void bios(uint8_t n, uint32_t at)
{
	switch (n) {
	case 0x08: { /* timer: count ticks at 0040:006C, run the user hook INT 1Ch, EOI */
		uint32_t t = (uint32_t)mrd(0x46c, 4) + 1;
		mwr(0x46c, 4, t);
		uint16_t seg = (uint16_t)mrd(0x1c * 4 + 2, 2), off = (uint16_t)mrd(0x1c * 4, 2);
		if (!(seg == BIOS_SEG && off == 0x1c)) {
			wr(R_CS, 2, BIOS_SEG);
			interrupt(0x1c, BIOS_SEG * 16 + 0x100, BIOS_SEG * 16 + 0x100);
		}
		eoi();
		break;
	}
	case 0x09: /* keyboard: the scancode into the BIOS key buffer */
		bios_keyboard();
		eoi();
		break;
	case 0x1c: /* user timer hook: nothing */
		break;
	default:
		dos_int(n, at);
		break;
	}
	uint16_t ip = pop16(), cs = pop16(), f = pop16();
	wr(R_CS, 2, cs);
	if (n == 0x08 || n == 0x09 || n == 0x1c) {
		set_flags_word(f); /* IRET */
	}
	else {
		/* services return results in the flags (CF), like DOS's RETF 2: keep them, but restore
		 * the caller's interrupt, trap and direction flags */
		uint16_t keep = get_flags() & ~0x0700;
		set_flags_word((uint16_t)(keep | (f & 0x0700)));
	}
	(void)ip; /* the C caller continues at the right place */
}

int bios_entry(uint32_t linear)
{
	if (linear < BIOS_SEG * 16 || linear >= BIOS_SEG * 16 + 0x100) {
		return 0;
	}
	bios((uint8_t)(linear - BIOS_SEG * 16), linear);
	return 1;
}

/* ---- setup ---- */

/* tty: also take keys typed in the terminal (when there is no window) */
void pc_init(int tty)
{
	for (int n = 0; n < 256; n++) {
		mwr(n * 4, 2, (uint64_t)n);
		mwr(n * 4 + 2, 2, BIOS_SEG);
	}
	mwr(0x410, 2, 0x0021);   /* equipment: 80x25 colour, math chip absent */
	mwr(0x413, 2, 640);      /* KB of conventional memory */
	M[0x449] = 0x03;         /* video mode */
	mwr(0x44a, 2, 80);       /* columns */
	M[0x484] = 24;           /* rows - 1 */
	mwr(KBUF_HEAD, 2, KBUF_START); /* empty key buffer from 0040:001E to 0040:003E */
	mwr(KBUF_TAIL, 2, KBUF_START);
	mwr(0x480, 2, KBUF_START);
	mwr(0x482, 2, KBUF_END);
	mwr(0x463, 2, 0x3d4);    /* CRT controller port (colour); +6 is the status register games poll */
	warp_start();            /* before anything takes the time */
	pic_mask = 0xb8;         /* timer, keyboard, cascade and COM enabled, like a booted PC */
	pit_set(65536);
	vga_init();
	opl_init();
	ems_init();
	const char *cycles = getenv("WC_CYCLES");
	if (cycles) atomic_store(&cycles_per_ms, strcmp(cycles, "max") ? strtoull(cycles, NULL, 10) : 0);
	const char *fps = getenv("WC_FPS");
	if (fps) atomic_store(&flight_fps, strtoull(fps, NULL, 10));

	parse_script(getenv("WC_KEYS"));
	if (tty && isatty(0) && !getenv("WC_KEYS")) {
		struct termios raw;
		tcgetattr(0, &kbd_saved);
		raw = kbd_saved;
		raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
		raw.c_cc[VMIN] = 0;
		raw.c_cc[VTIME] = 0;
		tcsetattr(0, TCSANOW, &raw);
		atexit(restore_tty);
		kbd_tty = 1;
	}
}
