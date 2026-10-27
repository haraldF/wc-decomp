/* Expanded memory (LIM EMS 4.0, INT 67h), as GOG's DOSBox offers it (ems=true). Without it the
 * game keeps all its resources in conventional memory, and launching from a Secret Missions
 * series, the launch animation's buffer ended up over the music that was playing: the music
 * stalled with all its notes held.
 * The page frame (4 pages of 16 KB at E000:0000) is ordinary emulated memory; mapping a page
 * copies the page that was there back to its store and the new one in. The game finds the
 * driver by its name at offset 0Ah of INT 67h's segment (F000, the BIOS entries). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wc_rt.h"

enum {
	PAGE = 0x4000,
	FRAME = 0xe0000,       /* E000:0000 */
	PHYS = 4,              /* pages in the frame */
	PAGES = 512,           /* 8 MB */
	HANDLES = 64,
	NONE = 0xffff,
};

static uint8_t *store;             /* PAGES pages */
static uint8_t page_used[PAGES];
static struct {
	int open;
	uint16_t count;
	uint16_t page[PAGES];          /* logical page -> store page */
	uint16_t saved[PHYS];          /* page map saved with AH=47h */
	int has_saved;
} handle[HANDLES];
static uint16_t mapped[PHYS] = { NONE, NONE, NONE, NONE }; /* physical page -> store page */

int ems_enabled(void)
{
	return store != NULL;
}

void ems_init(void)
{
	const char *e = getenv("WC_EMS");
	if (e && !strcmp(e, "0")) {
		return;
	}
	store = calloc(PAGES, PAGE);
	if (!store) {
		return;
	}
	memcpy(M + 0xf000a, "EMMXXXX0", 8);
	handle[0].open = 1; /* handle 0: the operating system's, no pages */
}

/* Put store page s (or nothing) into physical page p. */
static void map(int p, uint16_t s)
{
	if (mapped[p] == s) {
		return;
	}
	uint8_t *frame = M + FRAME + p * PAGE;
	if (mapped[p] != NONE) {
		memcpy(store + (size_t)mapped[p] * PAGE, frame, PAGE);
	}
	/* the same page in another physical page: take its current contents from there */
	for (int q = 0; q < PHYS; q++) {
		if (q != p && s != NONE && mapped[q] == s) {
			memcpy(store + (size_t)s * PAGE, M + FRAME + q * PAGE, PAGE);
		}
	}
	if (s != NONE) {
		memcpy(frame, store + (size_t)s * PAGE, PAGE);
	}
	mapped[p] = s;
}

static uint16_t free_pages(void)
{
	uint16_t n = 0;
	for (int i = 0; i < PAGES; i++) n += !page_used[i];
	return n;
}

static int valid(uint16_t h)
{
	return h < HANDLES && handle[h].open;
}

/* Give handle h n pages in all (more or fewer). Returns 0 or an EMS error code. */
static int resize(uint16_t h, uint16_t n)
{
	if (n > PAGES) return 0x87;
	if (n > handle[h].count && n - handle[h].count > free_pages()) return 0x88;
	while (handle[h].count > n) {
		uint16_t s = handle[h].page[--handle[h].count];
		for (int p = 0; p < PHYS; p++) {
			if (mapped[p] == s) mapped[p] = NONE;
		}
		page_used[s] = 0;
	}
	for (int s = 0; handle[h].count < n; s++) {
		if (!page_used[s]) {
			page_used[s] = 1;
			memset(store + (size_t)s * PAGE, 0, PAGE);
			handle[h].page[handle[h].count++] = (uint16_t)s;
		}
	}
	return 0;
}

static void result(int err)
{
	wr(R_AH, 1, (uint64_t)err);
}

void ems_int(uint32_t at)
{
	uint16_t dx = (uint16_t)rd(R_DX, 2), bx = (uint16_t)rd(R_BX, 2);
	uint8_t ah = (uint8_t)rd(R_AH, 1), al = (uint8_t)rd(R_AL, 1);
	if (!store) {
		result(0x80);
		return;
	}
	switch (ah) {
	case 0x40: /* status */
		result(0);
		break;
	case 0x41: /* page frame segment */
		wr(R_BX, 2, FRAME >> 4);
		result(0);
		break;
	case 0x42: /* free and total pages */
		wr(R_BX, 2, free_pages());
		wr(R_DX, 2, PAGES);
		result(0);
		break;
	case 0x43: { /* allocate BX pages */
		int h = 1;
		while (h < HANDLES && handle[h].open) h++;
		if (h == HANDLES) {
			result(0x85);
			break;
		}
		if (bx == 0) {
			result(0x89);
			break;
		}
		if (bx > free_pages()) {
			result(bx > PAGES ? 0x87 : 0x88);
			break;
		}
		memset(&handle[h], 0, sizeof handle[h]);
		handle[h].open = 1;
		resize((uint16_t)h, bx);
		wr(R_DX, 2, (uint64_t)h);
		result(0);
		break;
	}
	case 0x44: /* map logical page BX of handle DX into physical page AL (BX FFFFh: unmap) */
		if (!valid(dx)) result(0x83);
		else if (al >= PHYS) result(0x8b);
		else if (bx == NONE) { map(al, NONE); result(0); }
		else if (bx >= handle[dx].count) result(0x8a);
		else { map(al, handle[dx].page[bx]); result(0); }
		break;
	case 0x45: /* deallocate */
		if (!valid(dx) || dx == 0) {
			result(dx == 0 ? 0 : 0x83);
			break;
		}
		resize(dx, 0);
		handle[dx].open = 0;
		result(0);
		break;
	case 0x46: /* version 4.0 */
		wr(R_AL, 1, 0x40);
		result(0);
		break;
	case 0x47: /* save the page map for handle DX */
		if (!valid(dx)) { result(0x83); break; }
		if (handle[dx].has_saved) { result(0x8d); break; }
		memcpy(handle[dx].saved, mapped, sizeof mapped);
		handle[dx].has_saved = 1;
		result(0);
		break;
	case 0x48: /* restore it */
		if (!valid(dx)) { result(0x83); break; }
		if (!handle[dx].has_saved) { result(0x8e); break; }
		for (int p = 0; p < PHYS; p++) map(p, handle[dx].saved[p]);
		handle[dx].has_saved = 0;
		result(0);
		break;
	case 0x4b: { /* open handles */
		uint16_t n = 0;
		for (int h = 0; h < HANDLES; h++) n += handle[h].open != 0;
		wr(R_BX, 2, n);
		result(0);
		break;
	}
	case 0x4c: /* pages of handle DX */
		if (!valid(dx)) { result(0x83); break; }
		wr(R_BX, 2, handle[dx].count);
		result(0);
		break;
	case 0x4e: { /* get/set the whole page map (ES:DI, DS:SI): 4 words */
		uint32_t to = (uint32_t)rd(R_ES, 2) * 16 + (uint32_t)rd(R_DI, 2);
		uint32_t from = (uint32_t)rd(R_DS, 2) * 16 + (uint32_t)rd(R_SI, 2);
		if (al == 3) { wr(R_AL, 1, PHYS * 2); result(0); break; }
		if (al > 3) { result(0x8f); break; }
		if (al == 0 || al == 2) {
			for (int p = 0; p < PHYS; p++) mwr(to + 2 * p, 2, mapped[p]);
		}
		if (al == 1 || al == 2) {
			for (int p = 0; p < PHYS; p++) {
				uint16_t s = (uint16_t)mrd(from + 2 * p, 2);
				map(p, s < PAGES && page_used[s] ? s : NONE);
			}
		}
		result(0);
		break;
	}
	case 0x51: /* reallocate handle DX to BX pages */
		if (!valid(dx)) { result(0x83); break; }
		{
			int err = resize(dx, bx);
			wr(R_BX, 2, handle[dx].count);
			result(err);
		}
		break;
	default:
		if (wc_trace) fprintf(stderr, "wc: unimplemented EMS function AH=%02x at %05x\n", ah, at);
		result(0x84);
		break;
	}
	if (wc_trace) {
		fprintf(stderr, "wc: INT 67h AH=%02x AL=%02x BX=%04x DX=%04x -> AH=%02llx BX=%04llx DX=%04llx\n", ah, al,
			bx, dx, rd(R_AH, 1), rd(R_BX, 2), rd(R_DX, 2));
	}
}
