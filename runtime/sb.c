/* Sound Blaster digital sound: the DSP at ports 220h-22Fh, IRQ 5, played through the 8237 DMA
 * controller (8-bit channels 0-3, ports 00h-0Fh and the page registers). Only what the original
 * Sound Blaster does: reset (2x6h, then AAh at 2xAh), 8-bit mono output in single-cycle (14h),
 * auto-initialize (1Ch) and direct (10h) mode at the rate of a time constant (40h), speaker, pause,
 * version. A block plays in emulated time, its end raises the IRQ, and sound.c mixes the samples
 * into whatever has the output (AdLib or MT-32), by time: sb_mix. WC_SB=0 removes the card.
 * The FM chip of the card is the AdLib's (opl.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wc_rt.h"

uint64_t pc_now_ns(void);
void opl_poll(uint64_t now);
void mt32_poll(uint64_t now);
extern int wc_irq_pending;

#define SB_BASE 0x220
#define SB_IRQ 5
#define SB_DMA 1

static int present = 1;

/* ---- DMA controller ---- */

static struct { uint16_t base_addr, base_count, addr, count; uint8_t page, mode; } dma[4];
static uint8_t dma_mask = 0x0f, dma_status, dma_flipflop;

static uint8_t dma_page_channel(uint64_t port)
{
	switch (port) {
	case 0x87: return 0;
	case 0x83: return 1;
	case 0x81: return 2;
	case 0x82: return 3;
	}
	return 4;
}

/* ---- DSP ---- */

static uint8_t replies[16];
static unsigned replies_len;
static uint8_t cmd, args[2];
static unsigned args_have, args_need;
static uint8_t time_constant = 0xa5;   /* about 11 kHz */
static int speaker;

/* the block being played */
static int playing, paused, auto_init;
static uint32_t rate, bytes, block_len = 0x800; /* block_len: for auto-initialize */
static uint64_t start_ns, paused_at_ns;
static int16_t direct;                   /* the last sample of 10h */

static void reply(uint8_t v)
{
	if (replies_len < sizeof replies) replies[replies_len++] = v;
}

/* The samples up to now are mixed with the state they were played with. */
static void flush(uint64_t now)
{
	opl_poll(now);
	mt32_poll(now);
}

static uint64_t block_end_ns(void)
{
	return start_ns + (uint64_t)bytes * 1000000000u / rate;
}

static void start_block(uint64_t now, uint32_t len)
{
	rate = 1000000u / (256u - time_constant);
	bytes = len;
	start_ns = now;
	playing = 1;
	paused = 0;
	if (wc_trace) fprintf(stderr, "wc: sb: %u bytes at %u Hz from %05x\n", len, rate,
		(unsigned)dma[SB_DMA].page << 16 | dma[SB_DMA].addr);
}

/* The DMA channel's address and count after a block (the program may read them back). */
static void dma_advance(uint32_t n)
{
	dma[SB_DMA].addr += (uint16_t)n;
	dma[SB_DMA].count -= (uint16_t)n;
	dma_status |= 1 << SB_DMA; /* terminal count */
	if (dma[SB_DMA].mode & 0x10) { /* auto-initialize */
		dma[SB_DMA].addr = dma[SB_DMA].base_addr;
		dma[SB_DMA].count = dma[SB_DMA].base_count;
	}
}

static void command(uint64_t now)
{
	switch (cmd) {
	case 0x10: /* direct output: one sample */
		direct = (int16_t)((args[0] - 128) << 8);
		break;
	case 0x14: /* 8-bit single-cycle DMA output, length - 1 */
		start_block(now, (uint32_t)(args[0] | args[1] << 8) + 1);
		auto_init = 0;
		break;
	case 0x1c: /* 8-bit auto-initialize DMA output, the block size of 48h */
		start_block(now, block_len);
		auto_init = 1;
		break;
	case 0x48: /* block size for auto-initialize */
		block_len = (uint32_t)(args[0] | args[1] << 8) + 1;
		break;
	case 0x20: /* direct input: silence */
		reply(0x80);
		break;
	case 0x40: time_constant = args[0]; break;
	case 0xd0: /* pause */
		if (playing && !paused) {
			paused = 1;
			paused_at_ns = now;
		}
		break;
	case 0xd4: /* continue */
		if (playing && paused) {
			start_ns += now - paused_at_ns;
			paused = 0;
		}
		break;
	case 0xda: auto_init = 0; break; /* exit auto-initialize after this block */
	case 0xd1: speaker = 1; break;
	case 0xd3: speaker = 0; break;
	case 0xd8: reply(speaker ? 0xff : 0x00); break;
	case 0xe1: reply(2); reply(1); break; /* version 2.01 */
	case 0xf2: /* raise the interrupt */
		wc_irq_pending |= 1 << SB_IRQ;
		break;
	}
}

static unsigned arg_count(uint8_t c)
{
	switch (c) {
	case 0x10: case 0x40: case 0x38: case 0xe0: case 0xe4: return 1;
	case 0x14: case 0x16: case 0x17: case 0x24: case 0x48: case 0x74: case 0x75: case 0x76: case 0x77: return 2;
	}
	return 0;
}

static void dsp_write(uint8_t v)
{
	uint64_t now = pc_now_ns();
	flush(now);
	if (args_need) {
		args[args_have++] = v;
		if (args_have == args_need) {
			args_need = 0;
			command(now);
		}
		return;
	}
	cmd = v;
	args_have = 0;
	args_need = arg_count(v);
	if (!args_need) command(now);
}

static void dsp_reset(void)
{
	flush(pc_now_ns());
	playing = paused = auto_init = 0;
	speaker = 0;
	args_need = 0;
	direct = 0;
	replies_len = 0;
	reply(0xaa);
}

/* ---- the program's side ---- */

int sb_port(uint64_t port)
{
	return present && ((port >= SB_BASE + 6 && port <= SB_BASE + 0xe) || port <= 0x0f || dma_page_channel(port) < 4);
}

uint64_t sb_in(uint64_t port)
{
	if (port <= 0x07) { /* address and count of channel port / 2, low byte then high byte */
		int c = (int)port / 2;
		uint64_t now = pc_now_ns();
		uint16_t v = port & 1 ? dma[c].count : dma[c].addr;
		if (c == SB_DMA && playing && !paused && now < block_end_ns()) { /* where the block is now */
			uint16_t done = (uint16_t)((now - start_ns) * rate / 1000000000u);
			v = port & 1 ? (uint16_t)(v - done) : (uint16_t)(v + done);
		}
		uint8_t b = dma_flipflop ? v >> 8 : v & 0xff;
		dma_flipflop ^= 1;
		return b;
	}
	if (port == 0x08) {
		uint8_t s = dma_status;
		dma_status &= 0xf0; /* reading clears the terminal counts */
		return s;
	}
	if (port <= 0x0f || dma_page_channel(port) < 4) return 0xff;
	switch (port - SB_BASE) {
	case 0xa: /* read data */
		if (!replies_len) return 0xff;
		{
			uint8_t v = replies[0];
			memmove(replies, replies + 1, --replies_len);
			return v;
		}
	case 0xc: return 0x7f; /* write status: never busy */
	case 0xe: /* read status (data waiting); also acknowledges the interrupt */
		return replies_len ? 0xff : 0x7f;
	}
	return 0xff;
}

void sb_out(uint64_t port, uint64_t value)
{
	uint8_t v = (uint8_t)value;
	if (port <= 0x07) {
		int c = (int)port / 2;
		uint16_t *base = port & 1 ? &dma[c].base_count : &dma[c].base_addr;
		*base = dma_flipflop ? (uint16_t)((*base & 0x00ff) | v << 8) : (uint16_t)((*base & 0xff00) | v);
		if (port & 1) dma[c].count = *base;
		else dma[c].addr = *base;
		dma_flipflop ^= 1;
		return;
	}
	unsigned page = dma_page_channel(port);
	if (page < 4) {
		dma[page].page = v;
		return;
	}
	switch (port) {
	case 0x0a: /* single mask bit */
		if (v & 4) dma_mask |= (uint8_t)(1 << (v & 3));
		else dma_mask &= (uint8_t)~(1 << (v & 3));
		return;
	case 0x0b: dma[v & 3].mode = v; return;
	case 0x0c: dma_flipflop = 0; return;
	case 0x0d: dma_mask = 0x0f; dma_flipflop = 0; dma_status = 0; return; /* master clear */
	case 0x0e: dma_mask = 0; return;
	case 0x0f: dma_mask = v & 0x0f; return;
	}
	if (port <= 0x0f) return;
	switch (port - SB_BASE) {
	case 0x6:
		if (v & 1) dsp_reset();
		break;
	case 0xc: dsp_write(v); break;
	}
}

/* At the end of a block: advance the DMA channel, raise the interrupt, go on if auto-initialize. */
void sb_poll(uint64_t now)
{
	if (!playing || paused || now < block_end_ns()) return;
	uint64_t end = block_end_ns();
	dma_advance(bytes);
	wc_irq_pending |= 1 << SB_IRQ;
	if (auto_init) start_block(end, block_len);
	else playing = 0;
}

/* The sample at time t (16-bit), linearly between the DMA bytes. */
static int sample_at(uint64_t t)
{
	if (!playing || paused || t < start_ns || !speaker) return speaker ? direct : 0;
	uint64_t pos = (t - start_ns) * rate * 256u / 1000000000u; /* in 1/256 bytes */
	uint32_t i = (uint32_t)(pos >> 8);
	if (i >= bytes) return 0;
	if (dma_mask & (1 << SB_DMA)) return 0;
	uint32_t page = (uint32_t)dma[SB_DMA].page << 16;
	uint16_t a = dma[SB_DMA].addr;
	int s0 = M[page | (uint16_t)(a + i)] - 128;
	int s1 = i + 1 < bytes ? M[page | (uint16_t)(a + i + 1)] - 128 : s0;
	int f = (int)(pos & 0xff);
	return ((s0 * (256 - f) + s1 * f) >> 8) << 7; /* full scale +-16384: the AdLib is far quieter */
}

/* Adds the card's output to n stereo frames at rate, the first one at time t0. */
void sb_mix(int16_t *frames, unsigned n, unsigned out_rate, uint64_t t0)
{
	if (!present || (!playing && !direct)) return;
	for (unsigned i = 0; i < n; i++) {
		int s = sample_at(t0 + (uint64_t)i * 1000000000u / out_rate);
		for (int c = 0; c < 2; c++) {
			int v = frames[2 * i + c] + s;
			frames[2 * i + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
	}
}

void sb_init(void)
{
	const char *on = getenv("WC_SB");
	present = !on || strcmp(on, "0");
}
