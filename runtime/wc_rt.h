/* Runtime for the C that WcRecomp.java generates: an emulated 8086 state and the helpers the
 * translated p-code uses. R[] is Ghidra's x86 register space (byte offsets below), M[] the 1 MB
 * real-mode address space. All values are carried as uint64_t and masked to their size. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Ghidra x86 register space offsets */
enum {
	R_AX = 0x0, R_AL = 0x0, R_AH = 0x1, R_CX = 0x4, R_CL = 0x4, R_CH = 0x5,
	R_DX = 0x8, R_DL = 0x8, R_DH = 0x9, R_BX = 0xc, R_BL = 0xc, R_BH = 0xd,
	R_SP = 0x10, R_BP = 0x14, R_SI = 0x18, R_DI = 0x1c,
	R_ES = 0x100, R_CS = 0x102, R_SS = 0x104, R_DS = 0x106,
	R_CF = 0x200, R_PF = 0x202, R_AF = 0x204, R_ZF = 0x206, R_SF = 0x207,
	R_TF = 0x208, R_IF = 0x209, R_DF = 0x20a, R_OF = 0x20b, R_IP = 0x284,
};

#define MEM_SIZE 0x100000u
extern uint8_t R[0x400];
extern uint8_t M[MEM_SIZE + 16];
extern uint64_t wc_icount;
extern int wc_trace;

/* The generated code was produced for the image at 1000:0000; the runtime may load it elsewhere
 * (like DOS does). wc_reloc is the load segment minus 0x1000; relocated constants add it. */
extern int64_t wc_reloc;
extern uint32_t wc_image_lo, wc_image_hi; /* runtime linear range of the image */

/* The recompiled program (WC.EXE or SM2.EXE): its file name and start, from its EXE header
 * (wc_program.c, generated). */
struct wc_program {
	const char *exe;
	uint16_t cs, ip, ss, sp;
};
extern const struct wc_program wc_program;

/* Code the program loads at runtime (drivers) is recompiled from images captured in memory, each
 * at its own virtual segment from DRIVER_VIRT_SEG on; it runs at whatever segment it was loaded. */
#define DRIVER_VIRT_SEG 0x6000u
struct wc_driver {
	const char *name;
	uint32_t virt;                      /* code address of the driver's offset 0 */
	uint32_t count;                     /* instructions */
	const uint32_t *addr;               /* their code addresses, sorted */
	void (*const *func)(uint32_t);      /* the C function containing each */
	uint32_t nranges;
	const uint16_t (*ranges)[2];        /* offset and length of the instruction bytes */
	const uint8_t *image;               /* the captured 64 KB */
};

/* Code address (of the generated code) for a runtime linear address reached with the current CS,
 * and the reverse for an instruction of the code running now. */
uint32_t code_addr(uint64_t linear);
uint32_t runtime_addr(uint32_t code);

static inline uint64_t rd(uint32_t off, int size) { uint64_t v = 0; memcpy(&v, R + off, size); return v; }
static inline void wr(uint32_t off, int size, uint64_t v) { memcpy(R + off, &v, size); }

void mem_write_hook(uint32_t addr, int size);
extern uint32_t wc_watch; /* WC_WATCH: report writes to this linear address (0 = off) */
extern uint32_t wc_last_ins;
void watch_hit(uint32_t addr, int size, uint64_t value);
static inline uint64_t mrd(uint64_t addr, int size) { uint64_t v = 0; memcpy(&v, M + (addr & 0xfffff), size); return v; }
extern uint8_t wc_codemap[MEM_SIZE / 8]; /* bytes of recompiled driver instructions */
void code_write(uint32_t addr);
static inline void mwr(uint64_t addr, int size, uint64_t v)
{
	uint32_t a = (uint32_t)(addr & 0xfffff);
	memcpy(M + a, &v, size);
	uint32_t e = (a + size - 1) & 0xfffff;
	if (((wc_codemap[a >> 3] >> (a & 7)) | (wc_codemap[e >> 3] >> (e & 7))) & 1) {
		code_write(a);
	}
	if (wc_watch && wc_watch - a < (uint32_t)size) {
		watch_hit(a, size, v);
	}
	if (a >= 0xa0000) {
		mem_write_hook(a, size);
	}
}

/* Overlay code's reads through CS: from the pristine (relocated) program image, since the heap
 * overlaps the flattened overlays' addresses at runtime. */
extern uint8_t *wc_image_copy;
static inline uint64_t mrd_code(uint64_t addr, int size)
{
	uint32_t a = (uint32_t)(addr & 0xfffff);
	if (a - wc_image_lo + (uint32_t)size <= wc_image_hi - wc_image_lo) {
		uint64_t v = 0;
		memcpy(&v, wc_image_copy + (a - wc_image_lo), size);
		return v;
	}
	return mrd(addr, size);
}

/* real-mode segment:offset -> linear address */
static inline uint64_t lin(uint64_t seg, uint64_t off) { return (((seg & 0xffff) << 4) + (off & 0xffff)) & 0xfffff; }

static inline int64_t sx(uint64_t v, int size) { int sh = 64 - 8 * size; return (int64_t)(v << sh) >> sh; }
static inline uint64_t mask(int size) { return size >= 8 ? ~0ULL : (1ULL << (8 * size)) - 1; }
static inline uint64_t shl(uint64_t v, uint64_t n) { return n >= 64 ? 0 : v << n; }
static inline uint64_t shr(uint64_t v, uint64_t n) { return n >= 64 ? 0 : v >> n; }
static inline uint64_t sar(uint64_t v, uint64_t n, int size) { return (uint64_t)(sx(v, size) >> (n >= 63 ? 63 : n)); }
static inline uint64_t carry(uint64_t a, uint64_t b, int size) { return ((a & mask(size)) + (b & mask(size))) > mask(size); }
static inline uint64_t scarry(uint64_t a, uint64_t b, int size)
{
	int64_t r = sx(a, size) + sx(b, size);
	return r != sx((uint64_t)r, size);
}
static inline uint64_t sborrow(uint64_t a, uint64_t b, int size)
{
	int64_t r = sx(a, size) - sx(b, size);
	return r != sx((uint64_t)r, size);
}

void cpu_report(FILE *f); /* instructions executed and registers */
_Noreturn void cpu_fail(const char *what, uint32_t at);
void set_flags_word(uint16_t flags); /* the 16-bit FLAGS value into CF, PF, ..., OF */
_Noreturn void cpu_div0(uint32_t at);
static inline uint64_t udiv(uint64_t a, uint64_t b, uint32_t at) { if (!b) cpu_div0(at); return a / b; }
static inline uint64_t urem(uint64_t a, uint64_t b, uint32_t at) { if (!b) cpu_div0(at); return a % b; }
static inline uint64_t sdiv(uint64_t a, uint64_t b, int size, uint32_t at)
{
	if (!b) cpu_div0(at);
	return (uint64_t)(sx(a, size) / sx(b, size));
}
static inline uint64_t srem(uint64_t a, uint64_t b, int size, uint32_t at)
{
	if (!b) cpu_div0(at);
	return (uint64_t)(sx(a, size) % sx(b, size));
}

/* Enter the recompiled code: run() at a runtime linear address (computed by the program, e.g. an
 * indirect call), run_code() at an address of the 1000:0000 image (static jumps and calls). */
void run(uint32_t linear);
void run_code(uint32_t code);
/* INT n at code address at; next is the code address of the following instruction */
void cpu_int(uint64_t n, uint32_t at, uint32_t next);
uint64_t port_in(uint64_t port, int size);
void port_out(uint64_t port, uint64_t value, int size);
void trace_ins(uint32_t at);

/* Shadow return stack: each translated call records where it expects to continue, so that a
 * jump to that address (a hand-made return) can return from the C function instead. */
extern uint32_t wc_ret_stack[];
extern int wc_ret_depth;
#define WC_RET_STACK 8192
static inline void push_ret(uint32_t code)
{
	if (wc_ret_depth >= WC_RET_STACK) cpu_fail("call stack overflow at", code);
	wc_ret_stack[wc_ret_depth++] = code;
}
static inline void pop_ret(void) { wc_ret_depth--; }
static inline int is_return(uint32_t code) { return wc_ret_depth > 0 && wc_ret_stack[wc_ret_depth - 1] == code; }

/* Before every instruction: count it, deliver pending hardware interrupts (checked every 1024
 * instructions, or right away while one is pending), trace. */
extern int wc_irq_pending;
void irq_poll(uint32_t at);
#define TRACE_INS(a) (wc_last_ins = (a), ++wc_icount, (wc_irq_pending || !(wc_icount & 0x3ff)) ? irq_poll(a) : (void)0, \
	wc_trace >= 2 ? trace_ins(a) : (void)0)

/* registers by name, for the runtime */
#define AX rd(R_AX, 2)
#define BX rd(R_BX, 2)
#define CX rd(R_CX, 2)
#define DX rd(R_DX, 2)
#define DL rd(R_DL, 1)
#define AL rd(R_AL, 1)
#define AH rd(R_AH, 1)
#define BL rd(R_BL, 1)
#define DS rd(R_DS, 2)
#define ES rd(R_ES, 2)
static inline void set_cf(int on) { wr(R_CF, 1, on ? 1 : 0); }
