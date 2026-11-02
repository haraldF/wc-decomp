/* PC hardware around the CPU: interrupt delivery, the 8259 interrupt controller, the 8253 timer,
 * the keyboard controller and the ROM BIOS entry points (every interrupt vector starts out
 * pointing to F000:nn, handled here natively). */
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "host.h"
#include "net.h"
#include "wc_rt.h"

void dos_int(uint64_t n, uint32_t at);
uint64_t vga_in(uint64_t port);
uint64_t opl_in(uint64_t port);
void opl_out(uint64_t port, uint64_t value);
void opl_poll(uint64_t now);
void opl_init(void);
uint64_t mt32_in(uint64_t port);
void mt32_out(uint64_t port, uint64_t value);
void mt32_poll(uint64_t now);
void mt32_init(void);
int sb_port(uint64_t port);
uint64_t sb_in(uint64_t port);
void sb_out(uint64_t port, uint64_t value);
void sb_poll(uint64_t now);
void sb_init(void);
void wc_dump_memory(const char *why);
void ems_init(void);
uint8_t joystick_in(void);
void joystick_out(void);
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
#define BIOS_CONFIG 0xe6f5u /* system configuration table (INT 15h AH=C0h), where AT BIOSes have it */

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

/* Headless, emulated time comes from the instructions executed (WC_CLOCK=instructions, the default
 * with WC_HEADLESS; WC_CLOCK=real: real time): WC_CYCLES (or the program's default speed) of them
 * per ms, plus the time skipped where the game waits (a frame's slot in flight, a key). Runs go as
 * fast as the CPU emulation and the same every time, so WC_KEYS scripts land on the same frames. */
static int clock_instructions;
static uint64_t clock_rate, clock_skip_ns; /* instructions per ms; time skipped while waiting */

uint64_t pc_now_ns(void)
{
	if (clock_instructions) return wc_icount * 1000000u / clock_rate + clock_skip_ns;
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
	if ((w && !strcmp(w, "0")) || clock_instructions) return;
	warp_start_ns = real_ns();
	atomic_store(&warp_end_ns, 0);
}

static int view_child; /* this process only draws the friend's view (see view_spawn) */
static uint64_t view_start;

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
	{ "alt", 0, 0x38 }, { "ctrl", 0, 0x1d },
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

/* A key by its WC_KEYS name ("tab", "f1", "a", ...); 0 if there is none. */
static uint8_t key_named(const char *n)
{
	for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
		if (!strcmp(n, keys[i].name)) return keys[i].code;
	}
	return strlen(n) == 1 ? scancode_for(n[0]) : 0;
}

/* The keys a flight stick's buttons press, by the numbers printed on them (from 1): 3 Enter
 * (missile), 4 Tab (afterburner); WC_STICK_KEYS=11:tab,4:none changes them. Buttons 1 and 2 are
 * the joystick's own. */
static uint8_t stick_keys[33] = { [3] = 0x1c, [4] = 0x0f };

static void parse_stick_keys(const char *s)
{
	while (s && *s) {
		char name[16];
		int button, used = 0;
		if (sscanf(s, "%d:%15[^,]%n", &button, name, &used) != 2 || button < 1 || button > 32) {
			fprintf(stderr, "wc: bad WC_STICK_KEYS at \"%s\" (button:key,...)\n", s);
			exit(1);
		}
		uint8_t code = strcmp(name, "none") ? key_named(name) : 0;
		if (!code && strcmp(name, "none")) {
			fprintf(stderr, "wc: unknown key \"%s\" in WC_STICK_KEYS\n", name);
			exit(1);
		}
		stick_keys[button] = code;
		s += used + (s[used] == ',');
	}
}

/* The key button (from 0, as hosts count them) presses, 0 for none. */
uint8_t pc_stick_key(unsigned button)
{
	return button + 1 < sizeof stick_keys ? stick_keys[button + 1] : 0;
}

/* A key from this PC: with a friend, it waits until it is due on both PCs. */
static void key_out(uint8_t code)
{
	if (wc_net_on) net_local_key(code);
	else kbd_push(code);
}

void pc_net_key(uint8_t code)
{
	kbd_push(code);
}

static void press(uint8_t code)
{
	if (code) {
		key_out(code);
		key_out(code | 0x80);
	}
}

/* WC_KEYS="tick:key,...": keys to press after that many timer interrupts, for testing.
 * A key is a character or a name (esc, enter, space, up, ...); it is pressed and released at
 * once, or only pressed with a + in front (+up) and only released with a - (-up). The mouse:
 * @x.y moves it (320x200), lmb/rmb click (+lmb/-lmb hold and release); with WC_MOUSE=1. The
 * joystick: %x.y moves the stick (-100..100 each, 0 centred), %x.y.r twists it too (r likewise,
 * the roll), j1/j2 its buttons (+j1/-j1). */
enum { SCRIPT_MAX = 1024 };
static struct { uint64_t tick; uint8_t code; char how; int16_t mx, my; } script[SCRIPT_MAX];
static int script_mx = 160, script_my = 100, script_buttons, script_jx, script_jy, script_jroll, script_jbuttons;
static int script_len, script_pos;
static uint64_t ticks, script_jrelease;
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
		int mx, my, r = 0;
		if (sscanf(n, "@%d.%d", &mx, &my) == 2) { /* mouse to x.y */
			script[script_len].tick = t;
			script[script_len].how = 'm';
			script[script_len].mx = (int16_t)mx;
			script[script_len++].my = (int16_t)my;
			s += used + (s[used] == ',');
			continue;
		}
		if (sscanf(n, "%%%d.%d.%d", &mx, &my, &r) >= 2) { /* stick to x.y, twisted by r */
			script[script_len].tick = t;
			script[script_len].how = 'j';
			script[script_len].code = (uint8_t)(int8_t)r;
			script[script_len].mx = (int16_t)mx;
			script[script_len++].my = (int16_t)my;
			s += used + (s[used] == ',');
			continue;
		}
		if (!strcmp(n, "j1") || !strcmp(n, "j2")) { /* joystick button 1 or 2 */
			script[script_len].tick = t;
			script[script_len].how = how ? how : 'c';
			script[script_len].mx = -2;
			script[script_len++].code = n[1] == '1' ? 1 : 2;
			s += used + (s[used] == ',');
			continue;
		}
		if (!strcmp(n, "quit")) { /* end the run here (report, WC_SCREENSHOT) */
			script[script_len].tick = t;
			script[script_len++].how = 'q';
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
		uint8_t code = key_named(n);
		if (!code) {
			fprintf(stderr, "wc: unknown key \"%s\" in WC_KEYS\n", name);
			exit(1);
		}
		script[script_len].tick = t;
		script[script_len].how = how;
		script[script_len++].code = code;
		s += used + (s[used] == ',');
	}
	/* in time order, entries of the same tick as given (insertion sort: stable) */
	for (int i = 1; i < script_len; i++) {
		for (int j = i; j > 0 && script[j - 1].tick > script[j].tick; j--) {
			__typeof__(script[0]) e = script[j];
			script[j] = script[j - 1];
			script[j - 1] = e;
		}
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
	if (view_child) return;
	net_poll();
	while (script_pos < script_len && ticks >= script[script_pos].tick) {
		uint8_t code = script[script_pos].code;
		char how = script[script_pos].how;
		int16_t mx = script[script_pos].mx, my = script[script_pos++].my;
		if (how == 'q') {
			fflush(stdout);
			fprintf(stderr, "wc: quit by WC_KEYS at tick %llu\n", (unsigned long long)ticks);
			{ void friend_report(void); friend_report(); }
			cpu_report(stderr);
			wc_dump_memory("quit by WC_KEYS");
			host_exit(0);
		}
		if (how == 'm') {
			script_mx = mx;
			script_my = my;
			pc_mouse(script_mx, script_my, script_buttons);
		}
		else if (how == 'j') {
			script_jx = mx * 10;
			script_jy = my * 10;
			script_jroll = (int8_t)code * 10;
			pc_joystick(script_jx, script_jy, script_jroll, script_jbuttons);
		}
		else if (mx == -2) { /* joystick button, held for a little while when clicked */
			if (how != '-') pc_joystick(script_jx, script_jy, script_jroll, script_jbuttons |= code);
			if (how == '-') pc_joystick(script_jx, script_jy, script_jroll, script_jbuttons &= ~code);
			if (how == 'c') script_jrelease = ticks + 6;
		}
		else if (mx == -1) { /* mouse button: click, or + press / - release */
			if (how != '-') pc_mouse(script_mx, script_my, script_buttons |= code);
			if (how != '+') pc_mouse(script_mx, script_my, script_buttons &= ~code);
		}
		else if (how == '+') {
			key_out(code);
			script_held = code;
			script_repeat_ns = pc_now_ns() + 500000000u;
		}
		else if (how == '-') {
			key_out(code | 0x80);
			if (script_held == code) script_held = 0;
		}
		else press(code);
	}
	if (script_jrelease && ticks >= script_jrelease) { /* a clicked joystick button goes up */
		script_jrelease = 0;
		pc_joystick(script_jx, script_jy, script_jroll, script_jbuttons = 0);
	}
	if (script_held && pc_now_ns() >= script_repeat_ns) {
		key_out(script_held);
		script_repeat_ns += 92000000u;
	}
	uint32_t head = atomic_load_explicit(&host_keys_head, memory_order_relaxed);
	while (head != atomic_load_explicit(&host_keys_tail, memory_order_acquire)) {
		uint8_t code = host_keys[head % sizeof host_keys];
		uint64_t now = pc_now_ns();
		if (!(code & 0x80)) key_down_ns[code] = now;
		else if (now - key_down_ns[code & 0x7f] < MIN_KEY_NS) break; /* later keys wait behind it */
		key_out(code);
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
		mt32_poll(now);
		sb_poll(now); /* after the sound up to now: a block's end raises its interrupt */
	}
}

static void deliver(uint32_t at);

/* CPU speed: Wing Commander paces animation and space flight by CPU speed, so run at most
 * cycles_per_ms instructions per millisecond of real time, like DOSBox's "cycles". WC_CYCLES sets
 * it ("max": as fast as possible); the host changes it while running (pc_cycles).
 * In flight the game draws more or less per frame (enemies close by: more), so a fixed
 * instruction rate gives an uneven frame rate. There it runs at full speed and pc_frame() paces
 * the frames instead (flight_fps, WC_FPS; 0: no frame pacing). */
static _Atomic uint64_t cycles_per_ms; /* the program's default (wc_program.cycles), set in pc_init */
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
	if (wc_net_on) change = 0; /* both PCs must run alike */
	uint64_t c = faster(atomic_load(&cycles_per_ms), change, 500, 1000000);
	atomic_store(&cycles_per_ms, c);
	return c;
}

uint64_t pc_flight_fps(int change)
{
	if (wc_net_on) change = 0;
	uint64_t f = faster(atomic_load(&flight_fps), change, 5, 200);
	atomic_store(&flight_fps, f);
	return f;
}

int pc_in_flight(void)
{
	return atomic_load_explicit(&pacing_seen, memory_order_relaxed);
}

/* With a friend the emulated clock counts instructions, like headless, and follows real time here. */
static void net_pace(void)
{
	static uint64_t real0, emul0;
	uint64_t real = real_ns(), emul = pc_now_ns();
	if (!real0 || real > real0 + (emul - emul0) + 250000000u) { /* started, or held up: go on from here */
		real0 = real;
		emul0 = emul;
	}
	uint64_t due = real0 + (emul - emul0);
	if (due > real + 1000000u) {
		uint64_t wait = due - real;
		struct timespec ts = { 0, (long)(wait < 4000000u ? wait : 4000000u) };
		nanosleep(&ts, NULL);
	}
}

static void throttle(void)
{
	if (wc_net_on) {
		if (!view_child) net_pace();
		return;
	}
	uint64_t cycles = atomic_load_explicit(&cycles_per_ms, memory_order_relaxed);
	if (!cycles || clock_instructions) return;
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
		if (view_child && wc_icount - view_start > 40000000u) _exit(0); /* lost: the next one may do better */
		throttle();
		poll_devices();
	}
	deliver(at);
}

/* Experiment (WC_SWAP_*): put the player (object 0) and the wingman (object 1) in each other's place
 * in the game's per-object arrays at flight frame WC_SWAP_AT.
 *   WC_SWAP_TABLE=file  lines "offset stride" (hex): the arrays, by their DS offset
 *   WC_SWAP_MODE=once   stay swapped from there on
 *                fork   swap, run on for two frames, save the picture (prefix-fork.raw), and put
 *                       the machine back as it was, for a second view of the same game
 *   WC_SWAP_UNTIL=n     fork at every frame from WC_SWAP_AT to this one
 *   WC_SWAP_SHOT=prefix pictures of the frames around it (prefix-<frame>.raw, prefix-fork<frame>.raw)
 * Every frame logs a hash of the flight data, to compare runs with. */
void ems_snapshot(int restore);
extern uint8_t wc_palette[256][3];
static struct { uint16_t off, stride; } swap_table[256];
static int swap_count, swap_at = -1, swap_fork, swap_frame, swap_active, swap_every, swap_until;
static const char *swap_shot;
static uint8_t (*swap_m), swap_r[sizeof R];
static uint32_t *swap_rets;
static int swap_ret_depth;
static uintptr_t swap_native_sp;
static struct {
	uint64_t icount, irq_count[8], clock_skip_ns, pit_next_ns, pit_period_ns, pit_start_ns;
	uint64_t ticks, script_jrelease, throttle_start_ns, throttle_start_icount, next_frame_ns, frame_icount;
	uint8_t pic_mask, pic_in_service, kbd_queue[64], kbd_head, kbd_tail, kbd_data, script_held;
	int irq_pending, script_pos, script_mx, script_my, script_buttons, pacing;
} swap_saved;

/* The game's per-object arrays, by their DS offset and element size (from the decompiled code).
 * Not d1a2, the pilot: radio messages go by it, and only a wingman's are loaded. Nothing below
 * a9c2 either but the few that go by object (9e18, 9e2e, a9a2): the rest are other tables. Also out:
 * d1a0 and ca46 go by cockpit display (d1a0: the depth of its stack of screens), d240 and be58 by
 * place in a list, d416-d580 are the music's and the menus', c244 and c260 the campaign's, d24c the
 * damage to the player's systems (by system; other ships have none). */
static const struct { uint16_t off, stride; } swap_builtin[] = {
	{0x9e18,2}, {0x9e2e,2}, {0xa9a2,2}, {0xa9c2,12}, {0xacc4,2},
	{0xad44,2}, {0xadc4,12}, {0xae3c,1}, {0xae4c,2}, {0xae60,2}, {0xae75,1}, {0xaeb6,12},
	{0xb1b6,12}, {0xb4b6,12}, {0xb7b6,2}, {0xb7ca,2}, {0xb84a,1}, {0xb862,2}, {0xb8e2,2}, {0xb8f6,2},
	{0xb976,2}, {0xb9f6,4}, {0xbb02,2}, {0xbb8e,2}, {0xbd0e,1}, {0xbd1a,2}, {0xbd9a,2}, {0xbe26,2},
	{0xbe3a,2}, {0xbe4e,1}, {0xbe6c,4}, {0xbe94,2}, {0xbea8,1}, {0xbeb2,2}, {0xbf42,2},
	{0xbfc2,2}, {0xbfd6,1}, {0xbfe0,2}, {0xbff8,2}, {0xc078,1}, {0xc0ba,2}, {0xc16e,1}, {0xc17a,2},
	{0xc236,1}, {0xc284,1}, {0xc472,0x33}, {0xc28e,2}, {0xc30e,1}, {0xc34e,2}, {0xc364,2},
	{0xc378,2}, {0xc38c,2}, {0xc3a0,1}, {0xc3b0,2}, {0xc430,4}, {0xc458,1}, {0xc462,1}, 
	{0xc670,2}, {0xc68a,2}, {0xc69e,2}, {0xc6be,2}, {0xc6d2,2}, {0xc754,4}, {0xc854,1}, {0xc85e,8},
	{0xc8ae,1}, {0xc8b8,1}, {0xc8c2,2}, {0xc942,1}, {0xc94c,2}, {0xc960,2}, {0xc974,2}, {0xc988,4},
	{0xc9b0,1}, {0xca3c,1}, {0xca4c,1}, {0xca56,2}, {0xca6a,12}, {0xcae2,12}, {0xcde2,2},
	{0xce9a,12}, {0xd1ac,2}, {0xd22c,1}, {0xd236,1}, 
	{0xd3e6,2}, 
};

static uint32_t swap_ds(void) { return (uint32_t)((0x2231 + wc_reloc) * 16); }

static void swap_ships(void)
{
	uint8_t tmp[64];
	if (!swap_count) {
		for (size_t i = 0; i < sizeof swap_builtin / sizeof *swap_builtin; i++) {
			swap_table[swap_count].off = swap_builtin[i].off;
			swap_table[swap_count++].stride = swap_builtin[i].stride;
		}
	}
	for (int i = 0; i < swap_count; i++) {
		uint32_t a = swap_ds() + swap_table[i].off, n = swap_table[i].stride;
		memcpy(tmp, M + a, n);
		memcpy(M + a, M + a + n, n);
		memcpy(M + a + n, tmp, n);
	}
}

static void swap_picture(const char *what)
{
	char path[1024];
	snprintf(path, sizeof path, "%s-%s.raw", swap_shot, what);
	FILE *f = fopen(path, "wb");
	if (f) {
		fwrite(wc_palette, 1, sizeof wc_palette, f);
		fwrite(M + 0xa0000, 1, 320 * 200, f);
		fclose(f);
	}
}

#define SWAP_STATE(X) X(irq_count) X(clock_skip_ns) X(pit_next_ns) X(pit_period_ns) X(pit_start_ns) \
	X(ticks) X(script_jrelease) X(throttle_start_ns) X(throttle_start_icount) X(next_frame_ns) X(frame_icount) \
	X(pic_mask) X(pic_in_service) X(kbd_queue) X(kbd_head) X(kbd_tail) X(kbd_data) X(script_held) \
	X(script_pos) X(script_mx) X(script_my) X(script_buttons) X(pacing)

static void swap_save(void)
{
	if (!swap_m) {
		swap_m = malloc(sizeof M);
		swap_rets = malloc(WC_RET_STACK * sizeof *swap_rets);
	}
	memcpy(swap_m, M, sizeof M);
	memcpy(swap_r, R, sizeof R);
	memcpy(swap_rets, wc_ret_stack, wc_ret_depth * sizeof *swap_rets);
	swap_ret_depth = wc_ret_depth;
	swap_saved.icount = wc_icount;
	swap_saved.irq_pending = wc_irq_pending;
#define X(v) memcpy(&swap_saved.v, &v, sizeof v);
	SWAP_STATE(X)
#undef X
	ems_snapshot(0);
}

static void swap_restore(void)
{
	memcpy(M, swap_m, sizeof M);
	memcpy(R, swap_r, sizeof R);
	wc_ret_depth = swap_ret_depth;
	memcpy(wc_ret_stack, swap_rets, wc_ret_depth * sizeof *swap_rets);
	wc_icount = swap_saved.icount;
	wc_irq_pending = swap_saved.irq_pending;
#define X(v) memcpy(&v, &swap_saved.v, sizeof v);
	SWAP_STATE(X)
#undef X
	ems_snapshot(1);
}

static void swap_step(void)
{
	static int init;
	if (!init) {
		init = 1;
		const char *at = getenv("WC_SWAP_AT"), *table = getenv("WC_SWAP_TABLE"), *mode = getenv("WC_SWAP_MODE");
		swap_shot = getenv("WC_SWAP_SHOT");
		swap_fork = mode && !strcmp(mode, "fork");
		if (at) swap_at = atoi(at);
		const char *until = getenv("WC_SWAP_UNTIL"); /* fork at every frame up to this one */
		if (until) { swap_every = 1; swap_until = atoi(until); }
		FILE *f = table ? fopen(table, "r") : NULL;
		unsigned off, stride;
		while (f && swap_count < 256 && fscanf(f, "%x %x", &off, &stride) == 2) {
			swap_table[swap_count].off = (uint16_t)off;
			swap_table[swap_count++].stride = (uint16_t)stride;
		}
		if (f) fclose(f);
	}
	uintptr_t here = (uintptr_t)__builtin_frame_address(0);
	if (swap_active == 2) { /* the fork's second frame copy is on screen: the view from the wingman */
		if (swap_shot) {
			char what[32];
			snprintf(what, sizeof what, "fork%d", swap_frame - 2);
			swap_picture(what);
		}
		int same = here == swap_native_sp && wc_ret_depth == swap_ret_depth
			&& !memcmp(wc_ret_stack, swap_rets, wc_ret_depth * sizeof *swap_rets);
		if (!same) fprintf(stderr, "wc: swap: other call chain at frame %d (native sp %d, depth %d/%d)\n",
			swap_frame, here == swap_native_sp, wc_ret_depth, swap_ret_depth);
		swap_restore();
		swap_frame = swap_at + 1;
		swap_at = swap_every && swap_frame <= swap_until ? swap_frame : -1;
		swap_active = 0;
	} else if (swap_active == 1) {
		swap_active = 2;
		swap_frame++;
	} else {
		if (swap_frame == swap_at) {
			if (swap_fork) {
				swap_save();
				swap_native_sp = here;
				swap_ships();
				if (getenv("WC_SWAP_HURT")) { /* test: the wingman's damage, in the fork only (object 0 there) */
					static const int16_t hurt[] = { 10, 25, 5, 12, 20, 8 }; /* shields fore, aft; armor x 4 */
					memcpy(M + swap_ds() + 0xc988, hurt, 4);
					memcpy(M + swap_ds() + 0xc85e, hurt + 2, 8);
				}
				swap_active = 1;
			} else {
				swap_ships();
				fprintf(stderr, "wc: swap: %d arrays swapped at frame %d, tick %llu\n", swap_count, swap_frame, (unsigned long long)ticks);
			}
		}
		swap_frame++;
	}
	if (!swap_active && getenv("WC_SWAP_TABLE")) {
		uint64_t h = 0xcbf29ce484222325u;
		for (uint32_t a = swap_ds() + 0x9000; a < swap_ds() + 0xe000; a++) h = (h ^ M[a]) * 0x100000001b3u;
		fprintf(stderr, "wc: frame %d tick %llu icount %llu hash %016llx\n", swap_frame, (unsigned long long)ticks,
			(unsigned long long)wc_icount, (unsigned long long)h);
		if (swap_shot) {
			char what[32];
			snprintf(what, sizeof what, "%d", swap_frame);
			swap_picture(what);
		}
	}
}

/* Experiment (WC_FRIEND): a second player flies the wingman (object 1) instead of its AI.
 * WC_FRIEND=tick:x.y.roll.kps.fire,... from that tick on (x, y, roll -8..8; fire 0 or 1); the first
 * entry's tick is when the AI hands over. cli.ts's hooks (here patched into a copy of the generated
 * code) run at the entries of the AI (ovl09_28d0) and of the turning routine (ovl11_0606). */
enum { FRIEND_AI = 0x3aa20, FRIEND_TURN = 0x3b496, RADIO_QUEUE = 0x331fb, RADIO_MESSAGE = 0x34e20, GUNS = 0x3dd06, BURNER = 0x3dded, MISSILE = 0x3dc5e, OVL11_SEG = 0x3ae9,
	OVL02_SEG = 0x3030, DISPLAY_MODE = 0x30da8 /* ovl02_0aa8 */, DISPLAYS_DRAW = 0x3377f /* ovl02_347f */, WEAPONS_DRAW = 0x31333 /* ovl02_1033 */, SYSTEM_HIT = 0x310ec /* ovl02_0dec */ };
void run_code(uint32_t code);
static struct { uint64_t tick; int x, y, roll, kps, fire; } friend_script[64];
static int friend_len, friend_pos, friend_off = 1;
static uint16_t friend_state;
static int friend_took;
uint64_t friend_shots, friend_aimed;
static struct view_shared {
	_Atomic int current;
	_Atomic uint64_t when, pictures;
	_Atomic int have_cockpit, target, lock, target_display; /* the friend's, which only their view has */
	uint8_t pixels[2][320 * 200], palette[2][256 * 3];
} *view;
static uint64_t view_forks, view_fork_ns;
static uint64_t view_game_frames;
static int view_ahead_on, view_lead, view_frames, view_stick[3];
static uint64_t view_t0, view_period;
static int friend_sign_x = 1, friend_sign_y = 1;

static uint16_t ds_w(uint32_t off) { uint16_t v; memcpy(&v, M + swap_ds() + off, 2); return v; }
static void ds_set(uint32_t off, uint16_t v) { memcpy(M + swap_ds() + off, &v, 2); }

/* The friend's stick from their keys, each frame, as the game's keys step the player's (FUN_13be_0945,
 * a step for each frame a key is down): a real stick counts only while no key is steering. */
static int friend_stick[3], friend_last[3];

static int stick_step(int v, int back, int ahead, int shift)
{
	const int dir = ahead ? 1 : back ? -1 : 0;
	if (!dir || v * dir < 0) return 0;
	if (shift) return dir * 8;
	return v * dir < 8 ? v + dir : v; /* the game's own goes on to 9 and back: a bit faster, but unevenly */
}

static void friend_steer(int keys, int *x, int *y, int *roll)
{
	const int shift = keys & NET_KEY_SHIFT;
	friend_stick[0] = stick_step(friend_stick[0], keys & NET_KEY_LEFT, keys & NET_KEY_RIGHT, shift);
	friend_stick[1] = stick_step(friend_stick[1], keys & NET_KEY_DOWN, keys & NET_KEY_UP, shift);
	friend_stick[2] = stick_step(friend_stick[2], keys & NET_KEY_ROLL_LEFT, keys & NET_KEY_ROLL_RIGHT, shift);
	if (friend_stick[0]) *x = friend_stick[0];
	if (friend_stick[1]) *y = friend_stick[1];
	if (friend_stick[2]) *roll = friend_stick[2];
}

/* A routine of an overlay, called with two arguments: for the ones of overlay 11, the ship's number and one more. */
static void overlay_call(uint16_t seg, uint32_t code, uint16_t a, uint16_t b)
{
	static uint8_t saved[sizeof R];
	memcpy(saved, R, sizeof R);
	uint16_t sp = (uint16_t)rd(R_SP, 2), ss = (uint16_t)rd(R_SS, 2);
	uint32_t stack = ss * 16u;
	uint16_t frame[4] = { 0, (uint16_t)rd(R_CS, 2), a, b }; /* ip, cs, the arguments */
	sp -= 8;
	memcpy(M + stack + sp, frame, 8);
	wr(R_SP, 2, sp);
	wr(R_CS, 2, (uint64_t)(seg + wc_reloc) & 0xffff);
	run_code(code);
	memcpy(R, saved, sizeof R);
}

static void friend_call(uint32_t code, uint16_t ship, uint16_t arg)
{
	overlay_call(OVL11_SEG, code, ship, arg);
}

/* The guns, called as the player's Space key does (FUN_13be_0c0a). */
static void friend_guns(uint16_t ship)
{
	friend_call(GUNS, ship, 0);
	friend_shots++;
	if (getenv("WC_FRIEND_LOG")) fprintf(stderr, "wc: friend: guns at frame %d tick %llu\n", swap_frame, (unsigned long long)ticks);
}

/* The afterburner, as the player's Tab key does (ovl11_35d8), each frame it is held. */
static void friend_burner(uint16_t ship)
{
	const int32_t fuel = (int32_t)(ds_w(0xbe6c + 4 * ship) | (uint32_t)ds_w(0xbe6c + 4 * ship + 2) << 16);
	if (fuel <= 0) return;
	const int state = (int16_t)ds_w(0xca56 + 2 * ship), left = (int16_t)ds_w(0xc68a + 2 * ship);
	if (state == 1 && left > 2) return;
	friend_call(BURNER, ship, state == 1 && left ? 2 : 8);
}

void friend_report(void)
{
	if (friend_len || wc_net_on) fprintf(stderr, "wc: friend: %llu gun calls, %llu frames on target\n", (unsigned long long)friend_shots, (unsigned long long)friend_aimed);
	const char *shot = getenv("WC_VIEW_SHOT");
	FILE *f = shot && view ? fopen(shot, "wb") : NULL;
	if (f) {
		int i = atomic_load(&view->current);
		fwrite(view->palette[i], 1, sizeof view->palette[i], f);
		fwrite(view->pixels[i], 1, sizeof view->pixels[i], f);
		fclose(f);
	}
	if (view) fprintf(stderr, "wc: view: %llu forks, %llu pictures, %llu us per fork\n", (unsigned long long)view_forks,
		(unsigned long long)atomic_load(&view->pictures), (unsigned long long)(view_forks ? view_fork_ns / view_forks / 1000 : 0));
}

void friend_hook(uint32_t at)
{
	if (at == RADIO_MESSAGE) {
		if (getenv("WC_VIEW_LOG")) {
			uint16_t sender;
			memcpy(&sender, M + rd(R_SS, 2) * 16u + (uint16_t)(rd(R_SP, 2) + 4), 2);
			fprintf(stderr, "wc: %s radio message from object %d, tick %llu\n", view_child ? "view: own" : "game:", (int16_t)sender, (unsigned long long)ticks);
		}
		return;
	}
	if (at == RADIO_QUEUE) { /* the view says nothing of its own: the game's messages are on its screen already */
		if (view_child) memset(M + swap_ds() + 0xd236, 0xff, 10);
		return;
	}
	static int init;
	if (!init) {
		init = 1;
		const char *s = getenv("WC_FRIEND");
		const char *sign = getenv("WC_FRIEND_SIGN");
		if (sign) sscanf(sign, "%d,%d", &friend_sign_x, &friend_sign_y);
		while (s && *s && friend_len < 64) {
			int n = 0;
			unsigned long long t;
			if (sscanf(s, "%llu:%d.%d.%d.%d.%d%n", &t, &friend_script[friend_len].x, &friend_script[friend_len].y,
				&friend_script[friend_len].roll, &friend_script[friend_len].kps, &friend_script[friend_len].fire, &n) != 6) break;
			friend_script[friend_len++].tick = t;
			s += n + (s[n] == ',');
		}
	}
	int net_x, net_y, net_roll, net_throttle = 0, net_fire;
	const int from_net = net_friend(&net_x, &net_y, &net_roll, &net_throttle, &net_fire);
	uint16_t ship;
	memcpy(&ship, M + rd(R_SS, 2) * 16u + (uint16_t)(rd(R_SP, 2) + 4), 2);
	if (ship != 1) return;
	if (at == FRIEND_TURN && friend_took) { /* even if they stopped flying since the AI's turn */
		ds_set(0xbd1a + 2, friend_state);
		friend_took = 0;
	}
	if (view_child) { /* there the commander's ship is in this place: no AI, whose chatter would show */
		if (at == FRIEND_AI && (friend_state = ds_w(0xbd1a + 2)) == 12) {
			ds_set(0xbd1a + 2, 14);
			friend_took = 1;
		}
		return;
	}
	if (!from_net && (!friend_len || swap_active || ticks < friend_script[0].tick)) return;
	if (ds_w(0x662c) != 1) return; /* only a wingman, in object 1 */
	while (friend_pos + 1 < friend_len && ticks >= friend_script[friend_pos + 1].tick) friend_pos++;
	if (at == FRIEND_AI) {
		friend_state = ds_w(0xbd1a + 2);
		friend_off = friend_state != 12;
		if (!friend_off) { /* a state the AI's switch doesn't handle: no AI */
			ds_set(0xbd1a + 2, 14);
			friend_took = 1;
		}
		return;
	}
	if (friend_off) return;
	if (from_net) {
		friend_steer(net_friend_keys(), &net_x, &net_y, &net_roll);
		friend_last[0] = net_x;
		friend_last[1] = net_y;
		friend_last[2] = net_roll;
	}
	uint32_t type = ds_w(0xbff8 + 2) * 0x6fu;
	int x = from_net ? net_x : friend_script[friend_pos].x, y = from_net ? net_y : friend_script[friend_pos].y;
	int roll = from_net ? net_roll : friend_script[friend_pos].roll, fire = from_net ? net_fire : friend_script[friend_pos].fire;
	int guns_off = fire == 3;
	if (from_net && getenv("WC_FRIEND_CHASE")) { /* test: keeps that ship in the friend's view */
		const int32_t *me = (const int32_t *)(M + swap_ds() + 0xa9c2 + 12), *p = (const int32_t *)(M + swap_ds() + 0xa9c2 + 12 * atoi(getenv("WC_FRIEND_CHASE")));
		const int32_t *ax = (const int32_t *)(M + swap_ds() + 0xaeb6 + 12), *ay = (const int32_t *)(M + swap_ds() + 0xb1b6 + 12),
			*az = (const int32_t *)(M + swap_ds() + 0xb4b6 + 12);
		double rx = p[0] - me[0], ry = p[1] - me[1], rz = p[2] - me[2];
		double px = rx * ax[0] + ry * ax[1] + rz * ax[2], py = rx * ay[0] + ry * ay[1] + rz * ay[2], pz = rx * az[0] + ry * az[1] + rz * az[2];
		double a = pz > 0 ? pz : 1e-9;
		x = pz > 0 ? (int)(48 * px / a) : px >= 0 ? 8 : -8;
		y = (int)(48 * py / a);
		x = x > 8 ? 8 : x < -8 ? -8 : x;
		y = y > 8 ? 8 : y < -8 ? -8 : y;
	}
	if (fire == 2 || fire == 3) { /* test: stands in for a person, aiming at the nearest enemy (3: holding fire) */
		int best = -1;
		double bestd = 1e30, rx = 0, ry = 0, rz = 0;
		const int32_t *me = (const int32_t *)(M + swap_ds() + 0xa9c2 + 12);
		for (int i = 2; i < 10; i++) {
			if (ds_w(0xbd1a + 2 * i) < 11 || ds_w(0xbe94 + 2 * i) != 1) continue; /* alive, the enemy's side */
			const int32_t *p = (const int32_t *)(M + swap_ds() + 0xa9c2 + 12 * i);
			double dx = p[0] - me[0], dy = p[1] - me[1], dz = p[2] - me[2], d = dx * dx + dy * dy + dz * dz;
			if (d < bestd) { bestd = d; best = i; rx = dx; ry = dy; rz = dz; }
		}
		fire = 0;
		if (best >= 0) {
			const int32_t *ax = (const int32_t *)(M + swap_ds() + 0xaeb6 + 12), *ay = (const int32_t *)(M + swap_ds() + 0xb1b6 + 12),
				*az = (const int32_t *)(M + swap_ds() + 0xb4b6 + 12);
			double px = rx * ax[0] + ry * ax[1] + rz * ax[2], py = ry * ay[1] + rx * ay[0] + rz * ay[2],
				pz = rx * az[0] + ry * az[1] + rz * az[2];
			double a = pz > 0 ? pz : 1e-9;
			x = (int)(friend_sign_x * 8 * (px / a) * 6);
			y = (int)(friend_sign_y * 8 * (py / a) * 6);
			if (pz <= 0) x = friend_sign_x * (px >= 0 ? 8 : -8);
			if (x > 8) x = 8;
			if (x < -8) x = -8;
			if (y > 8) y = 8;
			if (y < -8) y = -8;
			double len2 = px * px + py * py;
			fire = pz > 0 && len2 < pz * pz * 0.0025 && bestd < 4e12; /* within ~3 degrees, in range */
			friend_aimed += fire;
			if (guns_off) fire = 0;
		}
	} else if (fire == 4) { /* test: flies to a point, WC_FRIEND_GOTO=x,y,z */
		double g[3] = { 0, 0, 0 };
		const char *to = getenv("WC_FRIEND_GOTO");
		if (to) sscanf(to, "%lf,%lf,%lf", &g[0], &g[1], &g[2]);
		const int32_t *me = (const int32_t *)(M + swap_ds() + 0xa9c2 + 12);
		double rx = g[0] * 65536 - me[0], ry = g[1] * 65536 - me[1], rz = g[2] * 65536 - me[2];
		const int32_t *ax = (const int32_t *)(M + swap_ds() + 0xaeb6 + 12), *ay = (const int32_t *)(M + swap_ds() + 0xb1b6 + 12),
			*az = (const int32_t *)(M + swap_ds() + 0xb4b6 + 12);
		double px = rx * ax[0] + ry * ax[1] + rz * ax[2], py = rx * ay[0] + ry * ay[1] + rz * ay[2], pz = rx * az[0] + ry * az[1] + rz * az[2];
		double a = pz > 0 ? pz : 1e-9;
		x = (int)(48 * px / a);
		y = (int)(48 * py / a);
		if (pz <= 0) x = px >= 0 ? 8 : -8;
		x = x > 8 ? 8 : x < -8 ? -8 : x;
		y = y > 8 ? 8 : y < -8 ? -8 : y;
		fire = 0;
	}
	/* as FUN_13be_0c4e turns the player's stick into turning speeds: both the current ones and those
	 * ovl11_0606 moves them to, at a limited rate that the player's don't know: a stale wanted speed held the turn back */
	const uint16_t pitch_rate = (uint16_t)((int16_t)ds_w(type + 0x49fe) * y / 8), yaw_rate = (uint16_t)(-(int16_t)ds_w(type + 0x49fc) * x / 8),
		roll_rate = (uint16_t)(-(int16_t)ds_w(type + 0x4a00) * roll / 8);
	ds_set(0xc6be + 2, pitch_rate);
	ds_set(0xc378 + 2, yaw_rate);
	ds_set(0xb8e2 + 2, roll_rate);
	ds_set(0xbf42 + 2, pitch_rate);
	ds_set(0xbd9a + 2, yaw_rate);
	ds_set(0xd1ac + 2, roll_rate);
	uint32_t set = (uint32_t)friend_script[friend_pos].kps * 128 / 5; /* the HUD's SET is this / 25.6 */
	if (from_net) { /* steps of 10 kps a frame up to the ship's top speed, as FUN_13be_1567 and ovl11_07eb */
		int32_t was = (int32_t)(ds_w(0xb9f6 + 4) | (uint32_t)ds_w(0xb9f6 + 6) << 16) + net_throttle * 256;
		const int32_t top = ds_w(0xae60 + 2) * 256;
		set = (uint32_t)(was < 0 ? 0 : was > top ? top : was);
	}
	ds_set(0xb9f6 + 4, (uint16_t)set);
	ds_set(0xb9f6 + 6, (uint16_t)(set >> 16));
	if (fire && ds_w(0xbb8e + 2) == 0xffff && (int16_t)ds_w(0xbe3a + 2) > 0) friend_guns(1);
	if (from_net && net_friend_burn()) friend_burner(1);
	static int missile_down;
	const int missile = from_net && (net_friend_keys() & NET_KEY_MISSILE);
	if (missile && !missile_down) { /* as the player's Enter key does (ovl11_2dce), at the target their view has */
		M[swap_ds() + 0xc284 + 1] = (uint8_t)net_friend_target();
		friend_call(MISSILE, 1, 0);
	}
	missile_down = missile;
}

/* The friend's view: each flight frame, a child process (fork, so nothing it does can touch the game
 * here) swaps the player and the wingman in its copy of the machine, runs on for two frames, leaves
 * the picture in shared memory and ends. The window shows the newest while they keep coming.
 * While the friend flies it also runs ahead of the game: their input takes the input delay to reach
 * it, so the child plays the blocks on their way and what they hold now, frame by frame on the game's
 * clock, until a frame has what they hold now. A wrong guess (the commander's next move) ends with the
 * child, and the next one starts from the real game. */
void mouse_view_reset(void);
void joystick_view_reset(void);
enum { VIEW_AHEAD_MAX = 8 }; /* frames, however long the input delay */
static pid_t view_pid;
static uint64_t view_spawned;

int pc_view_child(void) { return view_child; }

/* As FUN_13be_0d0d: steps by the distance from the middle of the view (tables at d1 and dd), full at its edges. */
void pc_mouse_stick(int x, int y, int *stick_x, int *stick_y)
{
	const int left = (int16_t)ds_w(0x89b0 + 0x1e66), top = (int16_t)ds_w(0x89b0 + 0x1e68), right = (int16_t)ds_w(0x89b0 + 0x1e6a), bottom = (int16_t)ds_w(0x89b0 + 0x1e6c);
	y = y > bottom ? bottom : y;
	const int dx = x - (right - left) / 2 + 1, dy = y - (bottom - top) / 2;
	int sx = 0, sy = 0;
	while (sx < 5 && (dx < 0 ? -dx : dx) >= (int16_t)ds_w(0xd1 + 2 * (uint32_t)sx)) sx++;
	while (sy < 5 && (dy < 0 ? -dy : dy) >= (int16_t)ds_w(0xdd + 2 * (uint32_t)sy)) sy++;
	sx = dx < 0 ? -sx : sx;
	sy = dy < 0 ? -sy : sy;
	if (x - 4 <= left) sx = -8;
	if (right <= x + 4) sx = 8;
	if (y - 4 <= top) sy = -8;
	if (bottom <= y + 4) sy = 8;
	*stick_x = sx;
	*stick_y = sy;
}

int pc_view_target(void)
{
	if (!view || !atomic_load(&view->have_cockpit)) return -1;
	const int target = atomic_load(&view->target); /* in the view, the two are the other way round */
	return target == 0 ? 1 : target == 1 ? 0 : target;
}

/* T and L go through the game's own key code in the child, and their result to the next child: the
 * game never hears of them. */
static int view_key;

/* A wingman's displays are its own: on the commander's the friend would see what they have chosen, their
 * missiles and what their damage has blanked. The weapons on the left and on the right the navigation,
 * or the target after T, each child draws anew; a radio message's face stays. */
static void view_displays(void)
{
	if (!(int8_t)M[swap_ds() + 0xd1a0]) overlay_call(OVL02_SEG, DISPLAY_MODE, 0, 1);
	if (!(int8_t)M[swap_ds() + 0xd1a1]) overlay_call(OVL02_SEG, DISPLAY_MODE, 1, atomic_load(&view->target_display) ? 3 : 5);
	overlay_call(OVL02_SEG, DISPLAYS_DRAW, 0, 0);
	if (!(int8_t)M[swap_ds() + 0xd1a0]) overlay_call(OVL02_SEG, WEAPONS_DRAW, 0, 0); /* drawn only when the player's change */
}

/* In the child, before the rates become turning speeds: the friend's stick is the one that turns "the player". */
static unsigned view_rates_frames;
void view_rates(void)
{
	const char *hurt = getenv("WC_COMMANDER_HURT");
	if (hurt && !view_child && view_rates_frames++ == (unsigned)atoi(hurt)) /* test: guns, comms, the targeting computer */
		for (int i = 0; i < 9; i++) /* each system in the mask, through the routine a hit goes to; its third argument is 4 anyway */
			if (atoi(strchr(hurt, ':') ? strchr(hurt, ':') + 1 : "42") >> i & 1) overlay_call(OVL02_SEG, SYSTEM_HIT, (uint16_t)i, 4);
	if (!view_child && getenv("WC_COMMANDER_AIM")) { /* test: stands in for a commander who can shoot */
		int best = -1;
		double bestd = 1e30, rx = 0, ry = 0, rz = 0;
		const int32_t *me = (const int32_t *)(M + swap_ds() + 0xa9c2);
		for (int i = 2; i < 10; i++) {
			if ((int16_t)ds_w(0xbd1a + 2 * i) < 11 || ds_w(0xbe94 + 2 * i) != 1) continue;
			const int32_t *p = (const int32_t *)(M + swap_ds() + 0xa9c2 + 12 * i);
			double dx = p[0] - me[0], dy = p[1] - me[1], dz = p[2] - me[2], d = dx * dx + dy * dy + dz * dz;
			if (d < bestd) { bestd = d; best = i; rx = dx; ry = dy; rz = dz; }
		}
		if (best >= 0) {
			const int32_t *ax = (const int32_t *)(M + swap_ds() + 0xaeb6), *ay = (const int32_t *)(M + swap_ds() + 0xb1b6), *az = (const int32_t *)(M + swap_ds() + 0xb4b6);
			double px = rx * ax[0] + ry * ax[1] + rz * ax[2], py = rx * ay[0] + ry * ay[1] + rz * ay[2], pz = rx * az[0] + ry * az[1] + rz * az[2];
			double a = pz > 0 ? pz : 1e-9;
			int x = pz > 0 ? (int)(48 * px / a) : px >= 0 ? 8 : -8, y = (int)(48 * py / a);
			ds_set(0xc3ac, (uint16_t)(x > 8 ? 8 : x < -8 ? -8 : x));
			ds_set(0xc3aa, (uint16_t)(y > 8 ? 8 : y < -8 ? -8 : y));
		}
		return;
	}
	if (!view_child) return;
	memset(M + swap_ds() + 0xd24c, 0, 9);
	if (view_ahead_on) {
		int x, y, roll, throttle, fire, burn;
		const uint64_t at = view_t0 + (uint64_t)view_frames * view_period;
		if (view_frames) { /* the wingman's stick turns it the frame after, the player's the same frame: so the one from before */
			net_ahead(at - view_period, &view_stick[0], &view_stick[1], &view_stick[2], &throttle, &fire, &burn);
			friend_steer(net_ahead_keys(at - view_period), &view_stick[0], &view_stick[1], &view_stick[2]);
		}
		net_ahead(at, &x, &y, &roll, &throttle, &fire, &burn);
		/* what friend_hook does to the wingman in the game */
		const int32_t was = (int32_t)(ds_w(0xb9f6) | (uint32_t)ds_w(0xb9f6 + 2) << 16) + throttle * 256, top = ds_w(0xae60) * 256;
		const uint32_t set = (uint32_t)(was < 0 ? 0 : was > top ? top : was);
		ds_set(0xb9f6, (uint16_t)set);
		ds_set(0xb9f6 + 2, (uint16_t)(set >> 16));
		if (fire && ds_w(0xbb8e) == 0xffff && (int16_t)ds_w(0xbe3a) > 0) friend_guns(0);
		if (burn) friend_burner(0);
	}
	ds_set(0xc3aa, (uint16_t)view_stick[1]);
	ds_set(0xc3ac, (uint16_t)view_stick[0]);
	ds_set(0xc3ae, (uint16_t)view_stick[2]);
	int pointer_x, pointer_y;
	const int pointer = view_ahead_on && net_view_pointer(&pointer_x, &pointer_y); /* the pointer is the friend's, not the commander's */
	ds_set(0x00c6, pointer ? 1 : 0);
	if (pointer) {
		ds_set(0x84ee, (uint16_t)pointer_x);
		const int bottom = (int16_t)ds_w(0x89b0 + 0x1e6c);
		ds_set(0x84f0, (uint16_t)(pointer_y > bottom ? bottom : pointer_y));
		ds_set(0x84fb, 2);
	}
	if (!view_frames) view_displays();
	M[swap_ds() + 0x6609] = (uint8_t)view_key; /* nor their keys: the friend's T and L */
	view_key = 0;
	M[swap_ds() + 0x660a] = 0;
}

/* WC_VIEW_PRED: for comparing the view's friend, commander and first enemy with the game's, by frame:
 * in the view, the frame on the screen, which is the one before the last. */
static char view_shown[256], view_last[256];
static void view_log_ships(const char *who, uint64_t frame, int friend_ship)
{
	if (!getenv("WC_VIEW_PRED")) return;
	const int32_t *p = (const int32_t *)(M + swap_ds() + 0xa9c2), *a = (const int32_t *)(M + swap_ds() + 0xaeb6);
	const int f = friend_ship * 3, c = (1 - friend_ship) * 3;
	char line[256];
	const int32_t *fw = (const int32_t *)(M + swap_ds() + 0xb4b6), *up = (const int32_t *)(M + swap_ds() + 0xb1b6);
	snprintf(line, sizeof line, "wc: pred %s %llu real %llu friend %d %d %d axis %d %d %d cmd %d %d %d enemy %d %d %d speedset %d fw %d %d %d up %d %d %d\n", who, (unsigned long long)frame, (unsigned long long)(real_ns() / 1000),
		p[f], p[f + 1], p[f + 2], a[f], a[f + 1], a[f + 2], p[c], p[c + 1], p[c + 2], p[6], p[7], p[8], (int32_t)(ds_w(0xb9f6 + 4 * friend_ship) | (uint32_t)ds_w(0xb9f6 + 4 * friend_ship + 2) << 16), fw[f], fw[f + 1], fw[f + 2], up[f], up[f + 1], up[f + 2]);
	if (friend_ship == 0 && !strcmp(who, "view")) {
		memcpy(view_shown, view_last, sizeof view_shown);
		memcpy(view_last, line, sizeof view_last);
	}
	else fprintf(stderr, "%s", line);
}

static void view_log_input(void)
{
	static int was[3];
	if (!view_ahead_on || !getenv("WC_VIEW_PRED")) return;
	int x, y, roll, throttle, fire, burn;
	net_ahead(UINT64_MAX / 2, &x, &y, &roll, &throttle, &fire, &burn);
	if (x != was[0] || y != was[1] || roll != was[2]) fprintf(stderr, "wc: pred input real %llu frame %llu stick %d %d %d\n", (unsigned long long)(real_ns() / 1000), (unsigned long long)view_game_frames, x, y, roll);
	was[0] = x; was[1] = y; was[2] = roll;
}

static void view_publish(void)
{
	if (view_shown[0]) fprintf(stderr, "%s", view_shown);
	if (getenv("WC_VIEW_LOG")) /* a radio message without text, in this child's memory */
		for (uint32_t a = 0; a + 8 < 0xa0000; a++)
			if (!memcmp(M + a, ": (null)", 8)) {
				fprintf(stderr, "wc: view: radio message without text, tick %llu\n", (unsigned long long)ticks);
				break;
			}
	atomic_store(&view->target, (int8_t)M[swap_ds() + 0xc284]);
	atomic_store(&view->lock, ds_w(0x6641));
	if (!(int8_t)M[swap_ds() + 0xd1a1]) atomic_store(&view->target_display, ds_w(0xbf3a) == 3);
	atomic_store(&view->have_cockpit, 1);
	int next = !atomic_load(&view->current);
	if (getenv("WC_VIEW_PRED")) {
		int changed = 0;
		for (size_t i = 0; i < sizeof view->pixels[next]; i++) changed += M[0xa0000 + i] != view->pixels[!next][i];
		fprintf(stderr, "wc: pred picture real %llu changed %d target %d lock %d display %d\n", (unsigned long long)(real_ns() / 1000), changed, (int8_t)M[swap_ds() + 0xc284], ds_w(0x6641), ds_w(0xbf3a));
	}
	memcpy(view->pixels[next], M + 0xa0000, sizeof view->pixels[next]);
	memcpy(view->palette[next], wc_palette, sizeof view->palette[next]);
	atomic_store(&view->current, next);
	atomic_store(&view->when, real_ns());
	atomic_fetch_add(&view->pictures, 1);
}

static void view_spawn(void)
{
	if (!view) {
		view = mmap(NULL, sizeof *view, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
		if (view == MAP_FAILED) { view = NULL; return; }
	}
	if (view_pid) {
		int status = 0;
		if (waitpid(view_pid, &status, WNOHANG) == 0) {
			if (real_ns() - view_spawned < 1000000000u) return; /* still at it */
			kill(view_pid, SIGKILL);
			waitpid(view_pid, &status, 0);
		}
		if (status && getenv("WC_VIEW_LOG")) fprintf(stderr, "wc: view failed, status %x, tick %llu\n", status, (unsigned long long)ticks);
		view_pid = 0;
	}
	if (ds_w(0x662c) != 1 || ds_w(0xbd1a + 2) != 12) return; /* no wingman to see from */
	int x = 0, y = 0, roll = 0, throttle, fire;
	const int flying = net_friend(&x, &y, &roll, &throttle, &fire);
	const uint64_t fps = atomic_load_explicit(&flight_fps, memory_order_relaxed);
	const char *predict = getenv("WC_NET_PREDICT");
	view_ahead_on = flying && fps && !(predict && !strcmp(predict, "0"));
	view_lead = 0;
	if (view_ahead_on) {
		net_ahead_prepare();
		view_period = 1000000000u / fps;
		view_lead = (int)((net_ahead_ns() + view_period - 1) / view_period) + 1;
		if (view_lead > VIEW_AHEAD_MAX) view_lead = VIEW_AHEAD_MAX;
		view_t0 = pc_now_ns();
		/* the next frame starts at its slot (pc_frame) */
		if (pacing && view_t0 <= next_frame_ns + 4 * view_period && next_frame_ns + view_period > view_t0) view_t0 = next_frame_ns + view_period;
	}
	view_log_input();
	view_key = view_ahead_on ? net_view_key() : 0;
	fflush(NULL);
	const uint64_t before = real_ns();
	pid_t pid = fork();
	if (pid < 0) return;
	if (pid) {
		view_pid = pid;
		view_spawned = real_ns();
		view_fork_ns += view_spawned - before;
		view_forks++;
		return;
	}
	view_child = 1;
	view_start = wc_icount;
	if (view_ahead_on) clock_skip_ns += view_t0 - pc_now_ns();
	mouse_view_reset();
	joystick_view_reset();
	view_stick[0] = view_ahead_on ? friend_last[0] : x;
	view_stick[1] = view_ahead_on ? friend_last[1] : y;
	view_stick[2] = view_ahead_on ? friend_last[2] : roll;
	swap_ships();
	/* the player's damaged systems are the commander's: a wingman has none, and hears nothing of them */
	static const uint16_t damage_texts[] = { 0x838, 0xd5d, 0xd6e, 0xd7d };
	for (size_t i = 0; i < sizeof damage_texts / sizeof *damage_texts; i++) M[swap_ds() + damage_texts[i]] = 0;
	M[swap_ds() + 0x8d20 + 10] = 0; /* (what is up now) */
	/* the player's missile: the friend's first */
	int16_t missile = -1;
	for (int i = M[swap_ds() + 0xc472] - 1; i >= 0; i--) {
		uint16_t type;
		memcpy(&type, M + swap_ds() + 0xc473 + 5 * i, 2);
		if (ds_w(type * 0x6fu + 0x49de) == 0xb) missile = (int16_t)i;
	}
	ds_set(0x6632, (uint16_t)missile);
	const int wingman_pilot = (int8_t)M[swap_ds() + 0xd1a2 + 1];
	if (wingman_pilot >= 0 && wingman_pilot < 8 && !(int8_t)M[swap_ds() + 0xd1a1]) /* targeted, the commander goes by the name in the place of the pilot's, and they by the player's (the roster's last): but a radio message is the pilot's */
		ds_set(0x98ca + 2 * wingman_pilot, 0x9912 + 8 * 38);
	if (atomic_load(&view->have_cockpit)) {
		M[swap_ds() + 0xc284] = (uint8_t)atomic_load(&view->target);
		ds_set(0x6641, (uint16_t)atomic_load(&view->lock));
	}
	if (getenv("WC_VIEW_RADIO")) M[swap_ds() + 0xd236 + 1] = (uint8_t)atoi(getenv("WC_VIEW_RADIO")); /* test: object 1 says this */
	if (!flying) { /* the AI flies: its turning speeds as the stick that would give them (FUN_13be_0c4e) */
		uint32_t type = ds_w(0xbff8) * 0x6fu;
		const int pitch = (int16_t)ds_w(type + 0x49fe), yaw = (int16_t)ds_w(type + 0x49fc), spin = (int16_t)ds_w(type + 0x4a00);
		if (yaw) view_stick[0] = -(int16_t)ds_w(0xbd9a) * 8 / yaw;
		if (pitch) view_stick[1] = (int16_t)ds_w(0xbf42) * 8 / pitch;
		if (spin) view_stick[2] = -(int16_t)ds_w(0xd1ac) * 8 / spin;
	}
}

int pc_view_frame(uint8_t *pixels, uint8_t *palette)
{
	if (!view || real_ns() - atomic_load(&view->when) > 400000000u) return 0;
	int i = atomic_load(&view->current);
	memcpy(pixels, view->pixels[i], sizeof view->pixels[i]);
	memcpy(palette, view->palette[i], sizeof view->palette[i]);
	return 1;
}

/* A frame is done (in flight): wait for its slot, keeping devices and interrupts going. */
void pc_frame(uint32_t at)
{
	if (view_child) {
		view_frames++;
		view_log_ships("view", view_game_frames + (uint64_t)view_frames, 0);
		/* The frame before this one is on the screen: the one after the swap, or as far on as their
		 * input has reached, always the same number so that the pictures keep step. */
		if (view_frames >= 2 + view_lead) {
			view_publish();
			_exit(0);
		}
		if (view_ahead_on) { /* the frame's slot, as below */
			const uint64_t now = pc_now_ns(), slot = view_t0 + (uint64_t)view_frames * view_period;
			if (now < slot) clock_skip_ns += slot - now;
		}
		return;
	}
	if (wc_net_join) {
		view_game_frames++;
		view_log_ships("game", view_game_frames, 1);
		view_spawn();
		if (view_child) return;
	}
	if (wc_net_on && !wc_net_join) view_log_ships("host", ++view_game_frames, 0);
	swap_step();
	uint64_t fps = atomic_load_explicit(&flight_fps, memory_order_relaxed);
	if (!fps) {
		net_frame(pc_now_ns());
		return;
	}
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
		if (clock_instructions) { /* skip to the frame's slot */
			clock_skip_ns += next_frame_ns - now;
			poll_devices();
			deliver(at);
			continue;
		}
		uint64_t wait = next_frame_ns - now;
		struct timespec ts = { 0, (long)(wait < 1000000u ? wait : 1000000u) };
		nanosleep(&ts, NULL);
		poll_devices();
		deliver(at);
	}
	frame_icount = wc_icount;
	net_frame(next_frame_ns);
}

/* A BIOS or DOS service waiting (for a key): let time pass with interrupts enabled. */
static void pc_wait(uint32_t at)
{
	uint64_t saved_if = rd(R_IF, 1);
	wr(R_IF, 1, 1);
	if (clock_instructions) clock_skip_ns += 1000000u;
	else usleep(1000);
	poll_devices();
	deliver(at);
	wr(R_IF, 1, saved_if);
}

/* A DOS service taking as long as a disk would, with interrupts enabled. */
void pc_delay(uint32_t at, uint32_t ms)
{
	while (ms--) pc_wait(at);
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
	case 0x201: v = joystick_in(); break;
	case 0x64: v = kbd_head != kbd_tail ? 1 : 0; break;
	case 0x3c7: case 0x3c8: case 0x3c9: case 0x3da: v = vga_in(port); break;
	case 0x388: case 0x389: v = opl_in(port); break;
	case 0x330: case 0x331: v = mt32_in(port); break;
	default: if (sb_port(port)) v = sb_in(port); break;
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
	case 0x201: joystick_out(); break;
	case 0x388: case 0x389: opl_out(port, value); break;
	case 0x330: case 0x331: mt32_out(port, value); break;
	default: if (sb_port(port)) sb_out(port, value); break;
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

static void bios_keyboard(uint8_t scan)
{
	uint8_t code = scan & 0x7f, up = scan & 0x80, flags = M[SHIFT_FLAGS];
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
	case 0x09: { /* keyboard: the scancode into the BIOS key buffer, unless the keyboard intercept
		* (INT 15h AH=4Fh, a hook may change AL) clears CF; WC2 reads keys that way */
		uint8_t scan = kbd_data, keep = 1;
		uint16_t seg = (uint16_t)mrd(0x15 * 4 + 2, 2), off = (uint16_t)mrd(0x15 * 4, 2);
		if (!(seg == BIOS_SEG && off == 0x15)) {
			uint16_t ax = (uint16_t)rd(R_AX, 2);
			wr(R_AX, 2, 0x4f00 | scan);
			wr(R_CF, 1, 1);
			wr(R_CS, 2, BIOS_SEG);
			interrupt(0x15, BIOS_SEG * 16 + 0x100, BIOS_SEG * 16 + 0x100);
			scan = (uint8_t)rd(R_AL, 1);
			keep = (uint8_t)rd(R_CF, 1);
			wr(R_AX, 2, ax);
		}
		if (keep) bios_keyboard(scan);
		eoi();
		break;
	}
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
	/* the configuration INT 15h AH=C0h returns: 8 bytes, an AT (model FCh), with a second PIC, a
	 * clock and the keyboard intercept (INT 9 calls INT 15h AH=4Fh), as in DOSBox */
	static const uint8_t config[] = { 8, 0, 0xfc, 0, 0, 0x70, 0, 0, 0, 0 };
	memcpy(M + BIOS_SEG * 16 + BIOS_CONFIG, config, sizeof config);
	const char *clock = getenv("WC_CLOCK"), *headless = getenv("WC_HEADLESS"), *rate = getenv("WC_CYCLES");
	clock_instructions = clock ? !strcmp(clock, "instructions") : headless && *headless && strcmp(headless, "0");
	clock_rate = rate && strcmp(rate, "max") && strtoull(rate, NULL, 10) ? strtoull(rate, NULL, 10) : wc_program.cycles;
	warp_start();            /* before anything takes the time */
	pic_mask = 0xb8;         /* timer, keyboard, cascade and COM enabled, like a booted PC */
	pit_set(65536);
	vga_init();
	mt32_init(); /* before the AdLib: one of them has the sound */
	opl_init();
	sb_init();
	ems_init();
	atomic_store(&cycles_per_ms, wc_program.cycles);
	const char *cycles = getenv("WC_CYCLES");
	if (cycles) atomic_store(&cycles_per_ms, strcmp(cycles, "max") ? strtoull(cycles, NULL, 10) : 0);
	const char *fps = getenv("WC_FPS");
	if (fps) atomic_store(&flight_fps, strtoull(fps, NULL, 10));

	parse_script(getenv("WC_KEYS"));
	parse_stick_keys(getenv("WC_STICK_KEYS"));
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
