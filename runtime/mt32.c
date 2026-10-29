/* Roland MT-32: an MPU-401 at ports 330h (data) and 331h (status, commands) in UART mode, whose
 * MIDI goes to munt's libmt32emu (third_party/munt, LGPL 2.1+). WC_MT32=folder turns it on with the
 * ROM images found there (and in its subfolders); main.c then also tells the game to use it ("r").
 * WC_MT32_MACHINE picks the model (munt's names: mt32_1_07, cm32l_1_02, ...), else the first one
 * the folder has ROMs for, in the order below. Samples are made like the AdLib's (opl.c). */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mt32emu.h"
#include "wc_rt.h"

uint64_t pc_now_ns(void);
int pc_warping(void);
int sound_open(unsigned rate);
void sound_out(int16_t *frames, unsigned count, uint64_t t0);

/* the game was made for the first MT-32s; the CM-32L adds sound effects it may not use */
static const char *const machines[] = {
	"mt32_1_07", "mt32_1_06", "mt32_1_05", "mt32_1_04", "mt32_bluer",
	"mt32_2_07", "mt32_2_06", "mt32_2_04", "mt32_2_03", "cm32l_1_02", "cm32l_1_00", "cm32ln_1_00",
};

static mt32emu_context synth;
static int playing;
static unsigned rate;
static uint64_t start_ns, frames_done;
static int uart;             /* UART mode: data bytes are plain MIDI */
static uint8_t replies[8];   /* what the MPU-401 has for the program to read at 330h */
static unsigned replies_len;

int mt32_active(void)
{
	return synth != NULL;
}

/* Make the samples from where we left off up to now, as opl_poll does. The synth runs even when
 * nothing hears it (the quick start, no sound output): it works through its MIDI queue as it renders. */
void mt32_poll(uint64_t now)
{
	if (!synth) return;
	uint64_t due = (now - start_ns) * rate / 1000000000u;
	if (due > frames_done + rate / 10) frames_done = due - rate / 10; /* stalled: skip */
	int out = playing && !pc_warping();
	int16_t buf[2 * 512];
	while (frames_done < due) {
		uint32_t n = due - frames_done > 512 ? 512 : (uint32_t)(due - frames_done);
		mt32emu_render_bit16s(synth, buf, n);
		if (out) sound_out(buf, n, start_ns + frames_done * 1000000000u / rate);
		frames_done += n;
	}
}

static void reply(uint8_t v)
{
	if (replies_len < sizeof replies) replies[replies_len++] = v;
}

uint64_t mt32_in(uint64_t port)
{
	if (!synth) return 0xff; /* no card */
	if (port == 0x331) return replies_len ? 0x3f : 0xbf; /* bit 7 clear: data to read; bit 6 clear: ready */
	if (!replies_len) return 0xff;
	uint8_t v = replies[0];
	memmove(replies, replies + 1, --replies_len);
	return v;
}

void mt32_out(uint64_t port, uint64_t value)
{
	if (!synth) return;
	uint8_t v = (uint8_t)value;
	if (port == 0x330) {
		mt32_poll(pc_now_ns()); /* the sound up to this moment, before the message */
		mt32emu_parse_stream(synth, &v, 1);
		return;
	}
	if (wc_trace) fprintf(stderr, "wc: MPU-401 command %02x\n", v);
	if (v == 0xff) { /* reset: back to intelligent mode */
		uart = 0;
		replies_len = 0;
		reply(0xfe);
		return;
	}
	if (uart) return; /* UART mode takes no other commands */
	reply(0xfe); /* acknowledged */
	if (v == 0x3f) uart = 1;
	else if (v == 0xac) reply(0x15); /* version 1.5 */
	else if (v == 0xad) reply(0x01); /* revision */
}

/* Offers every file in dir (and one level of subfolders) to the synth as a ROM of machine. */
static void add_roms(const char *machine, const char *dir, int depth)
{
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char path[1024];
		struct stat st;
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		if (stat(path, &st)) continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth) add_roms(machine, path, depth - 1);
		}
		else if (st.st_size <= 2 * 1024 * 1024) {
			mt32emu_add_machine_rom_file(synth, machine, path);
		}
	}
	closedir(d);
}

void mt32_init(void)
{
	const char *dir = getenv("WC_MT32");
	if (!dir || !*dir) return;
	const char *only = getenv("WC_MT32_MACHINE");
	for (size_t i = 0; i < sizeof machines / sizeof *machines && !synth; i++) {
		const char *machine = only && *only ? only : machines[i];
		synth = mt32emu_create_context((mt32emu_report_handler_i){ 0 }, NULL);
		add_roms(machine, dir, 1);
		if (mt32emu_open_synth(synth) != MT32EMU_RC_OK) {
			mt32emu_free_context(synth);
			synth = NULL;
		}
		if (only && *only) break;
	}
	if (!synth) {
		fprintf(stderr, "wc: WC_MT32: no %s ROMs (control and PCM) in %s\n", only && *only ? only : "MT-32", dir);
		exit(1);
	}
	mt32emu_rom_info info;
	mt32emu_get_rom_info(synth, &info);
	fprintf(stderr, "wc: Roland %s\n", info.control_rom_description);
	rate = mt32emu_get_actual_stereo_output_samplerate(synth);
	playing = sound_open(rate);
	start_ns = pc_now_ns();
}
