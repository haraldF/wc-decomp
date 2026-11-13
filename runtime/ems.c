/* Expanded memory (LIM EMS 4.0, INT 67h), as GOG's DOSBox offers it (ems=true). Without it the
 * game keeps all its resources in conventional memory, and launching from a Secret Missions
 * series, the launch animation's buffer ended up over the music that was playing: the music
 * stalled with all its notes held.
 * The page frame (4 pages of 16 KB at E000:0000) is ordinary emulated memory; mapping a page
 * copies the page that was there back to its store and the new one in. The game finds the
 * driver by its name at offset 0Ah of INT 67h's segment (F000, the BIOS entries).
 *
 * Privateer (PRCD.EXE) expects Origin's own EMS driver, JEMM, which PRIV.EXE installs: its INT 15h
 * AX=1209h commands (BX = two letters) switch between plain real mode ("ms") and JEMM's mode ("MS"),
 * where the whole upper memory, A000:0000 to the end of the first MB, is a frame of 24 EMS pages
 * (the game keeps its heap there and draws into a back buffer). VGA memory and the BIOS are only
 * visible in plain real mode, where the usual 4-page frame at E000 applies. JEMM runs hardware
 * interrupt handlers in plain real mode (ems_irq).
 * In JEMM's mode the VGA memory is at FFFF:0010 instead, in the HMA (linear 100000h, the A20 line is
 * on): the game copies its frames there (JEMM pages it onto the VGA; in plain real mode the HMA is
 * memory of its own).
 * The game maps one page into two physical pages at once (a heap page and a window), which copying
 * can't keep in step: from its first JEMM call on, the pages are mapped for real (natively): the
 * store, the upper memory's real contents and the HMA are a shared file, and each 16 KB of the upper
 * memory and the HMA is an mmap of the page it shows. In the browser, switching swaps the upper memory's real contents
 * with the mapped pages, and aliased pages go wrong. vga.c shows the real contents either way. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __EMSCRIPTEN__
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "wc_rt.h"

enum {
	PAGE = 0x4000,
	FRAME = 0xe0000,       /* E000:0000 */
	PHYS = 4,              /* pages in the frame */
	JEMM_FRAME = 0xa0000,  /* JEMM's mode: A000:0000 to the end of the first MB */
	JEMM_PHYS = 24,
	UPPER = 0x60000,       /* bytes from A000:0000 to the end of the first MB */
	HMA = 0x100000,        /* FFFF:0010, past 1 MB: the VGA memory in JEMM's mode */
	HMA_PAGES = 4,
	JEMM_FLAG = 0x600,     /* JEMM's "mode on" byte (INT 15h "FF" points to it), in low memory */
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
	uint16_t saved[JEMM_PHYS];     /* page map saved with AH=47h (all of the current frame) */
	int has_saved;
} handle[HANDLES];
/* physical page -> store page: in plain real mode (the frame at E000) and in JEMM's mode */
static uint16_t frames[2][JEMM_PHYS];
static int jemm;                   /* in JEMM's mode */
static uint16_t *mapped = frames[0];
static uint8_t *upper;             /* in JEMM's mode: the real contents of the upper memory */
static uint32_t frame_base(void) { return jemm ? JEMM_FRAME : FRAME; }
static int nphys(void) { return jemm ? JEMM_PHYS : PHYS; }
/* Mapped for real: the file holds the store's pages, then the upper memory's real contents (24
 * pages); each 16 KB of the upper memory is an mmap of one of them. */
static int shared;
#ifndef __EMSCRIPTEN__
static int shared_fd = -1;
#endif
static uint8_t *real_upper;        /* the real contents, mapped once more for the screen */

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
	for (int i = 0; i < JEMM_PHYS; i++) frames[0][i] = frames[1][i] = NONE;
	upper = malloc(UPPER);
}

/* VGA memory as the screen shows it: not the EMS pages over it in JEMM's mode */
uint8_t *ems_vram(void)
{
	return shared ? real_upper : jemm ? upper : M + 0xa0000;
}

#ifndef __EMSCRIPTEN__
/* Map file page f (store pages, then the upper memory's real contents, then the HMA's) at linear a. */
static void page_map(uint32_t a, uint32_t f)
{
	if (mmap(M + a, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, shared_fd, (off_t)f * PAGE) == MAP_FAILED) {
		perror("wc: EMS page mmap");
		exit(1);
	}
}

/* The 16 KB of the upper memory at A000:0000 + slot pages: store page s, or its real contents. */
static void slot_map(int slot, uint16_t s)
{
	page_map(JEMM_FRAME + (uint32_t)slot * PAGE, s == NONE ? PAGES + (uint32_t)slot : s);
}

/* Every slot of the upper memory as the current mode maps it. */
static void remap_upper(void)
{
	int first = (FRAME - JEMM_FRAME) / PAGE;
	for (int slot = 0; slot < JEMM_PHYS; slot++) {
		uint16_t s = jemm ? frames[1][slot] : slot >= first && slot < first + PHYS ? frames[0][slot - first] : NONE;
		slot_map(slot, s);
	}
	for (int k = 0; k < HMA_PAGES; k++) { /* JEMM's mode: the VGA memory; else the HMA's own */
		page_map(HMA + (uint32_t)k * PAGE, PAGES + (jemm ? (uint32_t)k : JEMM_PHYS + (uint32_t)k));
	}
}

/* From copying to mapping for real (at the first JEMM call; WC1's two-player view forks, and must
 * not share its memory with the game, so not before). 0 if the host can't. */
static int go_shared(void)
{
	if (shared || PAGE % sysconf(_SC_PAGESIZE) || (uintptr_t)(M + JEMM_FRAME) % PAGE) return shared;
	char path[] = "/tmp/wc-ems-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) return 0;
	unlink(path);
	size_t size = (size_t)(PAGES + JEMM_PHYS + HMA_PAGES) * PAGE;
	uint8_t *file = ftruncate(fd, (off_t)size) ? MAP_FAILED : mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (file == MAP_FAILED) {
		close(fd);
		return 0;
	}
	for (int p = 0; p < nphys(); p++) { /* what is mapped now back to the store, then all of it into the file */
		if (mapped[p] != NONE) memcpy(store + (size_t)mapped[p] * PAGE, M + frame_base() + p * PAGE, PAGE);
	}
	memcpy(file, store, (size_t)PAGES * PAGE);
	memcpy(file + (size_t)PAGES * PAGE, jemm ? upper : M + JEMM_FRAME, UPPER);
	memcpy(file + (size_t)(PAGES + JEMM_PHYS) * PAGE, M + HMA, (size_t)HMA_PAGES * PAGE);
	free(store);
	store = file;
	real_upper = file + (size_t)PAGES * PAGE;
	shared_fd = fd;
	shared = 1;
	remap_upper();
	return 1;
}
#endif

/* Put store page s (or nothing) into physical page p. */
static void map(int p, uint16_t s)
{
	if (mapped[p] == s) {
		return;
	}
#ifndef __EMSCRIPTEN__
	if (shared) {
		mapped[p] = s;
		slot_map((int)(frame_base() - JEMM_FRAME) / PAGE + p, s);
		return;
	}
#endif
	uint8_t *frame = M + frame_base() + p * PAGE;
	if (mapped[p] != NONE) {
		memcpy(store + (size_t)mapped[p] * PAGE, frame, PAGE);
	}
	/* the same page in another physical page: take its current contents from there */
	for (int q = 0; q < nphys(); q++) {
		if (q != p && s != NONE && mapped[q] == s) {
			memcpy(store + (size_t)s * PAGE, M + frame_base() + q * PAGE, PAGE);
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
		for (int f = 0; f < 2; f++) {
			for (int p = 0; p < JEMM_PHYS; p++) {
				if (frames[f][p] == s) frames[f][p] = NONE;
			}
		}
		page_used[s] = 0;
	}
#ifndef __EMSCRIPTEN__
	if (shared) remap_upper(); /* pages gone from the frames */
#endif
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

/* Into JEMM's mode (on) or plain real mode: the pages mapped in the mode left go back to the store,
 * the upper memory's real contents are put aside or back, and the new mode's pages come in. */
static void jemm_mode(int on)
{
	if (on == jemm || !upper) return;
#ifndef __EMSCRIPTEN__
	if (shared) {
		jemm = on;
		mapped = frames[on];
		remap_upper();
		M[JEMM_FLAG] = (uint8_t)on;
		return;
	}
#endif
	for (int p = 0; p < nphys(); p++) {
		if (mapped[p] != NONE) memcpy(store + (size_t)mapped[p] * PAGE, M + frame_base() + p * PAGE, PAGE);
	}
	if (on) memcpy(upper, M + 0xa0000, UPPER);
	else memcpy(M + 0xa0000, upper, UPPER);
	jemm = on;
	mapped = frames[on];
	for (int p = 0; p < nphys(); p++) {
		if (mapped[p] != NONE) memcpy(M + frame_base() + p * PAGE, store + (size_t)mapped[p] * PAGE, PAGE);
	}
	M[JEMM_FLAG] = (uint8_t)on;
}

/* Around hardware interrupts (pc.c, mouse.c): JEMM runs their handlers in plain real mode (where the
 * game's EMS code takes its 4-page path and leaves its own map alone), and goes back after. */
void ems_irq(int enter)
{
	static int depth, was;
	if (enter) {
		if (!depth++) {
			was = jemm;
			jemm_mode(0);
		}
	}
	else if (!--depth) {
		jemm_mode(was);
	}
}

/* Map logical page log of handle h into physical page p (log FFFFh: unmap); an EMS result. */
static int map_page(uint16_t h, int p, uint16_t log)
{
	if (!valid(h)) return 0x83;
	if (p < 0 || p >= nphys()) return 0x8b;
	if (log == NONE) { map(p, NONE); return 0; }
	if (log >= handle[h].count) return 0x8a;
	map(p, handle[h].page[log]);
	return 0;
}

/* JEMM's INT 15h AX=1209h (PRIV.EXE's real-mode part dispatches them); 1 if it was one. */
int jemm_int15(uint32_t at)
{
	if (rd(R_AX, 2) != 0x1209 || !store) return 0;
#ifndef __EMSCRIPTEN__
	go_shared();
#endif
	wc_amask = 0x1fffff; /* JEMM comes with HIMEM: the A20 line is on */
	uint16_t bx = (uint16_t)rd(R_BX, 2), was = (uint16_t)jemm;
	switch (bx) {
	case 0x4143: /* "AC": installed? */
		wr(R_AX, 2, 0);
		wr(R_BX, 2, 0x1209);
		break;
	case 0x534d: /* "MS": into JEMM's mode */
		jemm_mode(1);
		wr(R_BX, 2, was);
		break;
	case 0x736d: /* "ms": into plain real mode, BX = whether JEMM's mode was on */
		jemm_mode(0);
		wr(R_BX, 2, was);
		break;
	case 0x4646: /* "FF": ES:DI = the mode byte */
		wr(R_ES, 2, JEMM_FLAG >> 4);
		wr(R_DI, 2, JEMM_FLAG & 15);
		wr(R_AX, 2, 0);
		break;
	default: /* "FS" (a far pointer in ES:DI), "SG", "RS", "LC", ...: accepted */
		wr(R_AX, 2, 0);
		break;
	}
	if (wc_trace) fprintf(stderr, "wc: JEMM \"%c%c\" at %05x (mode %d -> %d)\n", bx & 0xff, bx >> 8, at, was, jemm);
	return 1;
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
		wr(R_BX, 2, frame_base() >> 4);
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
		result(map_page(dx, al, bx));
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
		memcpy(handle[dx].saved, mapped, sizeof handle[dx].saved);
		handle[dx].has_saved = 1;
		result(0);
		break;
	case 0x48: /* restore it */
		if (!valid(dx)) { result(0x83); break; }
		if (!handle[dx].has_saved) { result(0x8e); break; }
		for (int p = 0; p < nphys(); p++) map(p, handle[dx].saved[p]);
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
	case 0x4e: { /* get/set the whole page map (ES:DI, DS:SI): a word for each physical page */
		uint32_t to = (uint32_t)rd(R_ES, 2) * 16 + (uint32_t)rd(R_DI, 2);
		uint32_t from = (uint32_t)rd(R_DS, 2) * 16 + (uint32_t)rd(R_SI, 2);
		if (al == 3) { wr(R_AL, 1, (uint64_t)nphys() * 2); result(0); break; }
		if (al > 3) { result(0x8f); break; }
		if (al == 0 || al == 2) {
			for (int p = 0; p < nphys(); p++) mwr(to + 2 * p, 2, mapped[p]);
		}
		if (al == 1 || al == 2) {
			for (int p = 0; p < nphys(); p++) {
				uint16_t s = (uint16_t)mrd(from + 2 * p, 2);
				map(p, s < PAGES && page_used[s] ? s : NONE);
			}
		}
		result(0);
		break;
	}
	case 0x50: { /* map CX pages of handle DX: DS:SI = (logical, physical page or AL=1: segment) pairs */
		uint32_t from = (uint32_t)rd(R_DS, 2) * 16 + (uint32_t)rd(R_SI, 2);
		uint16_t cx = (uint16_t)rd(R_CX, 2);
		int err = al > 1 ? 0x8f : 0;
		for (uint16_t i = 0; i < cx && !err; i++) {
			uint16_t log = (uint16_t)mrd(from + 4u * i, 2), p = (uint16_t)mrd(from + 4u * i + 2, 2);
			int phys = al ? ((int)p * 16 - (int)frame_base()) / PAGE : p;
			err = map_page(dx, phys, log);
		}
		result(err);
		break;
	}
	case 0xde: /* VCPI: the physical address of the 4 KB page CX (DE06h: the Sound Blaster driver's DMA
		 * buffer; memory isn't paged here), the debug registers (DE09h: Privateer's debugging) */
		if (al == 0x06) wr(R_DX, 4, (uint64_t)(uint16_t)rd(R_CX, 2) << 12);
		result(al == 0x06 || al == 0x09 ? 0 : 0x84);
		break;
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

/* Experiment (WC_SWAP_*, pc.c): keep or put back the store and the mapping. */
void ems_snapshot(int restore)
{
	static uint8_t *saved;
	static uint16_t saved_mapped[2][JEMM_PHYS];
	if (!store) return;
	if (!saved) saved = malloc((size_t)PAGES * PAGE);
	if (restore) {
		memcpy(store, saved, (size_t)PAGES * PAGE);
		memcpy(frames, saved_mapped, sizeof frames);
	} else {
		memcpy(saved, store, (size_t)PAGES * PAGE);
		memcpy(saved_mapped, frames, sizeof frames);
	}
}
