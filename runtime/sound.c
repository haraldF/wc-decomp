/* Where the sound goes: the host's output and, with WC_WAV=file, a WAV recording (also headless).
 * One device makes the sound at a time: the AdLib (opl.c) or the MT-32 (mt32.c); the Sound Blaster's
 * digital sound (sb.c) is mixed into it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

void sb_mix(int16_t *frames, unsigned n, unsigned out_rate, uint64_t t0);

static FILE *wav;
static uint32_t rate, wav_rate, wav_frames;
static int to_host;

static void wav_header(uint32_t frames)
{
	uint32_t bytes = frames * 4;
	uint8_t h[45] = "RIFF____WAVEfmt \x10\0\0\0\x01\0\x02\0________\x04\0\x10\0data____";
	uint32_t v[] = { 36 + bytes, wav_rate, wav_rate * 4, bytes };
	memcpy(h + 4, &v[0], 4);
	memcpy(h + 24, &v[1], 4);
	memcpy(h + 28, &v[2], 4);
	memcpy(h + 40, &v[3], 4);
	fseek(wav, 0, SEEK_SET);
	fwrite(h, 1, 44, wav);
	fseek(wav, 0, SEEK_END);
	fflush(wav);
}

static void wav_close(void)
{
	wav_header(wav_frames);
	fclose(wav);
}

/* Returns 0 when the sound goes nowhere. */
int sound_open(unsigned out_rate)
{
	rate = out_rate;
	to_host = host_audio_open(rate);
	const char *path = getenv("WC_WAV");
	if (path && (wav = fopen(path, "wb"))) {
		wav_rate = rate;
		wav_header(0);
		atexit(wav_close);
	}
	return to_host || wav;
}

/* 16-bit stereo frames, the first one made for emulated time t0 */
void sound_out(int16_t *frames, unsigned count, uint64_t t0)
{
	sb_mix(frames, count, rate, t0);
	if (to_host) host_audio(frames, count);
	if (wav) {
		fwrite(frames, 4, count, wav);
		wav_frames += count;
		if (!(wav_frames & 0xffff)) wav_header(wav_frames);
	}
}
