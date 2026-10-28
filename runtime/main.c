/* Loads and relocates the program image, sets up the DOS process the way DOS's EXE loader would,
 * and enters the recompiled code at the program's entry point.
 * Usage: wc [game folder [game arguments...]]  (sm2 likewise, for the Secret Missions 2)
 *   game arguments  passed to the game on its DOS command line, e.g. "Origin -k" (the developers'
 *                   mode, "Origin" spelled exactly so; -k: the player's ship takes no damage)
 *   WC_TRACE=1      trace DOS calls; 2 also every instruction; 3 also I/O ports
 *   WC_KEYS=tick:key,...  press keys after that many timer interrupts (else: keys typed in the terminal);
 *                   +key only presses, -key only releases
 *   WC_LOAD_SEG=hex load segment (default 0200, which leaves about as much memory as DOSBox)
 *   WC_POKE=addr:byte,...  patch memory before starting (linear hex addresses), for experiments
 *   WC_WATCH=addr          report every write to that linear address (hex)
 *   WC_HEADLESS=1   no window (keys from the terminal or WC_KEYS)
 *   WC_MOUSE=1      report a mouse without a window (for WC_KEYS mouse entries)
 *   WC_MOUSE_SPEED=x  the mouse's speed in full screen (default 2; Cmd+Left/Right in the window)
 *   WC_WAV=file     record the sound (AdLib) to a WAV file
 *   WC_CYCLES=n     instructions per millisecond (default 8334; "max": unlimited; Cmd+Up/Down in the window)
 *   WC_FPS=n        frames per second in space flight, where frames are paced instead (default 15; 0: off;
 *                   Cmd+Up/Down in flight)
 *   WC_STATS=1      print the frame rate in flight every second
 *   WC_EMS=0        no expanded memory (EMS, 8 MB by default, as in GOG's DOSBox)
 *   WC_WARP=0       no quick start (until the first picture, time runs 20 times as fast)
 *   WC_JOYSTICK=0   no joystick (by default a centred one, moved by a game controller) */
#include <libgen.h>
#include <limits.h>
#ifndef __EMSCRIPTEN__
#include <mach-o/dyld.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "host.h"
#include "wc_rt.h"

void pc_init(int tty);
extern const char *wc_game_dir;
extern uint16_t wc_psp_seg;
int dos_file_exists(const char *dos);

enum {
	BUILD_SEG = 0x1000,  /* segment the generated code was produced for */
	TOP_SEG = 0xa000,    /* end of conventional memory */
};

static size_t read_file(const char *path, uint8_t *dst, size_t max)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		exit(1);
	}
	size_t n = fread(dst, 1, max, f);
	fclose(f);
	return n;
}

/* Load image.bin at load_seg and apply the relocations in relocs.bin, as DOS would. */
static void load_image(const char *dir, unsigned load_seg)
{
	char path[PATH_MAX];
	snprintf(path, sizeof path, "%s/image.bin", dir);
	uint32_t base = load_seg * 16;
	size_t size = read_file(path, M + base, MEM_SIZE - base);
	if (!size) {
		fprintf(stderr, "wc: %s is empty\n", path);
		exit(1);
	}
	wc_reloc = (int64_t)load_seg - BUILD_SEG;
	wc_image_lo = base;
	wc_image_hi = base + (uint32_t)size;

	static uint32_t relocs[16384];
	snprintf(path, sizeof path, "%s/relocs.bin", dir);
	size_t n = read_file(path, (uint8_t *)relocs, sizeof relocs) / 4;
	for (size_t i = 0; i < n; i++) {
		uint32_t a = relocs[i] - BUILD_SEG * 16 + base;
		mwr(a, 2, mrd(a, 2) + (uint64_t)wc_reloc);
	}
	wc_image_copy = malloc(size);
	memcpy(wc_image_copy, M + base, size);
}

/* The arguments after the game folder, as the DOS command line (in the PSP, at most 126 characters). */
static void set_command_line(uint8_t *psp, int argc, char **argv)
{
	char line[127] = "";
	size_t n = 0;
	for (int i = 0; i < argc; i++) {
		int w = snprintf(line + n, sizeof line - n, " %s", argv[i]);
		if (w < 0 || n + (size_t)w >= sizeof line) {
			fprintf(stderr, "wc: the game arguments are longer than DOS allows (126 characters)\n");
			exit(1);
		}
		n += (size_t)w;
	}
	/* WC_MT32 (mt32.c): "r" picks the Roland; the game reads its arguments after the config's words */
	if (getenv("WC_MT32") && n + 2 < sizeof line) {
		memcpy(line + n, " r", 3);
		n += 2;
	}
	psp[0x80] = (uint8_t)n;
	memcpy(psp + 0x81, line, n);
	psp[0x81 + n] = 0x0d;
}

static void setup_process(unsigned load_seg, int argc, char **argv)
{
	unsigned psp_seg = load_seg - 0x10, env_seg = load_seg - 0x20;
	/* environment: variables, an empty string ending them, a count word and the program path
	 * (the overlay manager reads the program's overlays through that path) */
	static const char vars[] = "COMSPEC=C:\\COMMAND.COM\0\0\1\0C:\\";
	memcpy(M + env_seg * 16, vars, sizeof vars - 1);
	strcpy((char *)M + env_seg * 16 + sizeof vars - 1, wc_program.exe);

	uint8_t *psp = M + psp_seg * 16;
	psp[0] = 0xcd; /* INT 20h */
	psp[1] = 0x20;
	psp[2] = TOP_SEG & 0xff;
	psp[3] = TOP_SEG >> 8;
	psp[0x2c] = env_seg & 0xff;
	psp[0x2d] = env_seg >> 8;
	set_command_line(psp, argc, argv);
	wc_psp_seg = psp_seg;

	wr(R_CS, 2, load_seg + wc_program.cs);
	wr(R_IP, 2, wc_program.ip);
	wr(R_SS, 2, load_seg + wc_program.ss);
	wr(R_SP, 2, wc_program.sp);
	wr(R_DS, 2, psp_seg);
	wr(R_ES, 2, psp_seg);
	wr(R_IF, 1, 1);
}

static unsigned entry_seg;

static void game(void)
{
	run(entry_seg * 16 + wc_program.ip);
	fprintf(stderr, "wc: entry point returned\n");
	cpu_report(stderr);
}

static void on_signal(int sig)
{
	fprintf(stderr, "\nwc: stopped by signal %d\n", sig);
	cpu_report(stderr);
	_exit(128 + sig);
}

int main(int argc, char **argv)
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
#ifdef __EMSCRIPTEN__
	const char *dir = "/wc"; /* image.bin and relocs.bin are embedded there */
#else
	char exe[PATH_MAX];
	uint32_t size = sizeof exe;
	if (_NSGetExecutablePath(exe, &size) != 0) {
		snprintf(exe, sizeof exe, "%s", argv[0]);
	}
	const char *dir = dirname(exe);
#endif

	const char *trace = getenv("WC_TRACE");
	wc_trace = trace ? atoi(trace) : 0;
	const char *seg = getenv("WC_LOAD_SEG");
	unsigned load_seg = seg ? (unsigned)strtoul(seg, NULL, 16) : 0x0200;
	if (load_seg < 0x80 || load_seg > 0x6000) {
		fprintf(stderr, "wc: WC_LOAD_SEG must be between 0080 and 6000\n");
		return 1;
	}
	if (argc > 1) {
		wc_game_dir = argv[1];
	}
#ifndef __EMSCRIPTEN__
	if (!dos_file_exists(wc_program.exe)) { /* the game would end with "Abnormal program termination" */
		fprintf(stderr, "wc: no %s in %s; usage: %s <game folder> [arguments]\n", wc_program.exe,
			wc_game_dir, basename(argv[0]));
		return 1;
	}
#endif

	load_image(dir, load_seg);
	setup_process(load_seg, argc > 2 ? argc - 2 : 0, argv + 2);
	pc_init(!host_open());

	const char *watch = getenv("WC_WATCH");
	wc_watch = watch ? (uint32_t)strtoul(watch, NULL, 16) : 0;

	const char *poke = getenv("WC_POKE");
	for (const char *p = poke; p && *p;) {
		unsigned addr, value;
		int used = 0;
		if (sscanf(p, "%x:%x%n", &addr, &value, &used) != 2 || addr >= MEM_SIZE) {
			fprintf(stderr, "wc: bad WC_POKE at \"%s\"\n", p);
			return 1;
		}
		M[addr] = (uint8_t)value;
		p += used + (p[used] == ',');
	}

	entry_seg = load_seg + wc_program.cs;
	host_run(game);
}
