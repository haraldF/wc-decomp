/* AdLib: the OPL2 at ports 388h (index, status) and 389h (data), emulated by Nuked-OPL3
 * (third_party/Nuked-OPL3, LGPL 2.1), which runs in OPL2 mode until a program enables OPL3.
 * Samples are made on the emulation's thread up to the current real time whenever devices are
 * polled, so register writes land at about the right moment; the host plays them. */
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
#include "opl3.h"
#include "wc_rt.h"

uint64_t pc_now_ns(void);

#define OPL_RATE 49716u /* the chip's own sample rate (14.31818 MHz / 288) */

static opl3_chip chip;
static uint8_t index_reg;
static int playing;          /* there is somewhere for the samples to go */
static uint64_t start_ns, frames_done;
static FILE *wav;            /* WC_WAV: the sound recorded to a WAV file */
static uint32_t wav_frames;

/* ---- timers: status bits 7 (any), 6 (timer 1, 80 us steps), 5 (timer 2, 320 us steps) ---- */

static uint8_t timer_value[2], timer_control, status;
static uint64_t timer_start_ns[2];

static void timers(uint64_t now)
{
	for (int t = 0; t < 2; t++) {
		if (!(timer_control & (1 << t)) || (timer_control & (0x40 >> t))) continue; /* stopped or masked */
		uint64_t period = (256u - timer_value[t]) * (t ? 320000u : 80000u);
		if (now - timer_start_ns[t] >= period) status |= (uint8_t)(0x40 >> t) | 0x80;
	}
}

/* ---- sound ---- */

static void wav_header(uint32_t frames)
{
	uint32_t bytes = frames * 4;
	uint8_t h[45] = "RIFF____WAVEfmt \x10\0\0\0\x01\0\x02\0________\x04\0\x10\0data____";
	uint32_t v[] = { 36 + bytes, OPL_RATE, OPL_RATE * 4, bytes };
	memcpy(h + 4, &v[0], 4);
	memcpy(h + 24, &v[1], 4);
	memcpy(h + 28, &v[2], 4);
	memcpy(h + 40, &v[3], 4);
	fseek(wav, 0, SEEK_SET);
	fwrite(h, 1, 44, wav);
	fseek(wav, 0, SEEK_END);
	fflush(wav);
}

/* Make the samples from where we left off up to now (at most 0.1 s at once). */
void opl_poll(uint64_t now)
{
	timers(now);
	if (!playing) return;
	uint64_t due = (now - start_ns) * OPL_RATE / 1000000000u;
	if (due > frames_done + OPL_RATE / 10) frames_done = due - OPL_RATE / 10; /* stalled: skip */
	int16_t buf[2 * 512];
	while (frames_done < due) {
		uint32_t n = due - frames_done > 512 ? 512 : (uint32_t)(due - frames_done);
		for (uint32_t i = 0; i < n; i++) OPL3_Generate(&chip, buf + 2 * i);
		host_audio(buf, n);
		if (wav) {
			fwrite(buf, 4, n, wav);
			wav_frames += n;
			if (!(wav_frames & 0xffff)) wav_header(wav_frames);
		}
		frames_done += n;
	}
}

static void wav_close(void)
{
	wav_header(wav_frames);
	fclose(wav);
}

uint64_t opl_in(uint64_t port)
{
	if (port == 0x388) {
		timers(pc_now_ns());
		return status;
	}
	return 0xff;
}

void opl_out(uint64_t port, uint64_t value)
{
	if (port == 0x388) {
		index_reg = (uint8_t)value;
		return;
	}
	uint8_t v = (uint8_t)value;
	switch (index_reg) {
	case 0x02: timer_value[0] = v; break;
	case 0x03: timer_value[1] = v; break;
	case 0x04:
		if (v & 0x80) { /* reset the flags */
			status = 0;
			break;
		}
		for (int t = 0; t < 2; t++) {
			if ((v & (1 << t)) && !(timer_control & (1 << t))) timer_start_ns[t] = pc_now_ns();
		}
		timer_control = v;
		break;
	default:
		opl_poll(pc_now_ns()); /* the sound up to this moment is made with the old registers */
		OPL3_WriteReg(&chip, index_reg, v);
		break;
	}
}

void opl_init(void)
{
	OPL3_Reset(&chip, OPL_RATE);
	playing = host_audio_open(OPL_RATE);
	const char *path = getenv("WC_WAV");
	if (path && (wav = fopen(path, "wb"))) {
		wav_header(0);
		atexit(wav_close);
		playing = 1;
	}
	start_ns = pc_now_ns();
}
