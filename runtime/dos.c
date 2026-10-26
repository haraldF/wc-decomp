/* Software interrupts: a small DOS (INT 21h) on top of the host file system, plus BIOS stubs.
 * Anything not implemented stops the program with the function number, so the next piece of
 * work is obvious. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "host.h"
#include "wc_rt.h"

void cpu_report(FILE *f);

const char *wc_game_dir = ".";
uint16_t wc_psp_seg;

#define MAX_HANDLES 64
static int handles[MAX_HANDLES] = { 0, 1, 2, -1, -1 }; /* DOS handle -> host fd; 3 = AUX, 4 = PRN */
/* The host path of each open file and whether it was changed, for host_file_saved() on close. */
static char handle_path[MAX_HANDLES][1024];
static uint8_t handle_changed[MAX_HANDLES];
static uint32_t dta;
static char cwd[128]; /* current directory below the game folder, DOS style, no leading backslash */

static void init_handles(void)
{
	static int done;
	if (!done) {
		for (int i = 5; i < MAX_HANDLES; i++) {
			handles[i] = -1;
		}
		done = 1;
	}
}

static void fail_dos(int code)
{
	wr(R_AX, 2, code);
	set_cf(1);
}

static void ok(void)
{
	set_cf(0);
}

/* Read a NUL-terminated string at seg:off. */
static void get_string(uint64_t seg, uint64_t off, char *buf, size_t size)
{
	size_t i = 0;
	for (; i + 1 < size; i++) {
		buf[i] = (char)M[lin(seg, off + i)];
		if (!buf[i]) {
			break;
		}
	}
	buf[i] = 0;
}

/* Absolute DOS path (no drive, no leading backslash, no "." or "..") from a path relative to cwd. */
static void dos_absolute(const char *dos, char *out, size_t size)
{
	char joined[256];
	if (dos[0] && dos[1] == ':') {
		dos += 2;
	}
	if (dos[0] == '\\' || dos[0] == '/') {
		snprintf(joined, sizeof joined, "%s", dos);
	}
	else {
		snprintf(joined, sizeof joined, "%s\\%s", cwd, dos);
	}
	out[0] = 0;
	for (char *part = strtok(joined, "\\/"); part; part = strtok(NULL, "\\/")) {
		if (!strcmp(part, ".")) {
			continue;
		}
		if (!strcmp(part, "..")) {
			char *slash = strrchr(out, '\\');
			*(slash ? slash : out) = 0;
			continue;
		}
		size_t len = strlen(out);
		snprintf(out + len, size - len, "%s%s", len ? "\\" : "", part);
	}
}

/* DOS path -> host path under the game folder, matching each component case-insensitively. */
static void host_path(const char *rel, char *out, size_t size)
{
	char dos[256];
	dos_absolute(rel, dos, sizeof dos);
	snprintf(out, size, "%s", wc_game_dir);
	const char *p = dos;
	while (*p) {
		while (*p == '\\' || *p == '/') {
			p++;
		}
		const char *start = p;
		while (*p && *p != '\\' && *p != '/') {
			p++;
		}
		if (p == start) {
			break;
		}
		char part[256];
		snprintf(part, sizeof part, "%.*s", (int)(p - start), start);
		if (!strcmp(part, ".")) {
			continue;
		}
		DIR *d = opendir(out);
		struct dirent *e;
		if (d) {
			while ((e = readdir(d))) {
				if (!strcasecmp(e->d_name, part)) {
					snprintf(part, sizeof part, "%s", e->d_name);
					break;
				}
			}
			closedir(d);
		}
		size_t len = strlen(out);
		snprintf(out + len, size - len, "/%s", part);
	}
}

static int dos_error(void)
{
	switch (errno) {
	case ENOENT: return 2;
	case ENOTDIR: return 3;
	case EMFILE: return 4;
	case EACCES: case EPERM: return 5;
	default: return 0x1f;
	}
}

static int new_handle(int fd)
{
	init_handles();
	for (int i = 5; i < MAX_HANDLES; i++) {
		if (handles[i] < 0) {
			handles[i] = fd;
			return i;
		}
	}
	close(fd);
	return -1;
}

static int host_fd(uint64_t h)
{
	init_handles();
	return h < MAX_HANDLES ? handles[h] : -1;
}

static void open_file(int flags, const char *what)
{
	char dos[128], path[1024];
	get_string(DS, DX, dos, sizeof dos);
	host_path(dos, path, sizeof path);
	struct stat st;
	int fd = !dos[0] ? (errno = ENOENT, -1)
		: stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? (errno = EACCES, -1) /* DOS can't open folders */
		: open(path, flags, 0644);
	if (wc_trace) {
		fprintf(stderr, "wc: %s \"%s\" -> %s%s\n", what, dos, path, fd < 0 ? " (failed)" : "");
	}
	if (fd < 0) {
		fail_dos(dos_error());
		return;
	}
	int h = new_handle(fd);
	if (h < 0) {
		fail_dos(4);
		return;
	}
	snprintf(handle_path[h], sizeof handle_path[h], "%s", path);
	handle_changed[h] = (flags & O_TRUNC) != 0;
	wr(R_AX, 2, h);
	ok();
}

int bios_key_peek(uint16_t *key);
uint16_t bios_key_get(uint32_t at);
void bios_key_flush(void);

/* Console input: the character of the next key; for keys without one (arrows, F1, ...) 0 first,
 * then the scancode on the next call. */
static int extended_scan = -1;

static uint8_t console_char(uint32_t at)
{
	if (extended_scan >= 0) {
		uint8_t scan = (uint8_t)extended_scan;
		extended_scan = -1;
		return scan;
	}
	uint16_t key = bios_key_get(at);
	if ((key & 0xff) == 0) {
		extended_scan = key >> 8;
	}
	return (uint8_t)key;
}

static void dos(uint32_t at)
{
	uint8_t ah = (uint8_t)AH;
	if (wc_trace) {
		fprintf(stderr, "wc: INT 21h AH=%02x AL=%02llx BX=%04llx CX=%04llx DX=%04llx DS=%04llx at %05x\n", ah, AL, BX,
			CX, DX, DS, at);
	}
	switch (ah) {
	case 0x02: /* write character */
		putchar((int)rd(R_DL, 1));
		break;
	case 0x09: { /* write $-terminated string */
		for (uint64_t off = DX; M[lin(DS, off)] != '$'; off++) {
			putchar(M[lin(DS, off)]);
		}
		break;
	}
	case 0x01: /* read a character with echo */
		wr(R_AL, 1, console_char(at));
		putchar((int)AL);
		fflush(stdout);
		break;
	case 0x06: /* direct console I/O: DL = FF reads without waiting (ZF set if none) */
		if (DL == 0xff) {
			uint16_t key;
			int have = extended_scan >= 0 || bios_key_peek(&key);
			wr(R_ZF, 1, !have);
			wr(R_AL, 1, have ? console_char(at) : 0);
		}
		else {
			putchar((int)DL);
		}
		break;
	case 0x07: case 0x08: /* read a character without echo */
		wr(R_AL, 1, console_char(at));
		break;
	case 0x0b: /* input status: FF if a key is waiting */
		{
			uint16_t key;
			wr(R_AL, 1, extended_scan >= 0 || bios_key_peek(&key) ? 0xff : 0);
		}
		break;
	case 0x0c: /* flush the input, then read with function AL */
		bios_key_flush();
		extended_scan = -1;
		if (AL == 0x01 || AL == 0x06 || AL == 0x07 || AL == 0x08) {
			wr(R_AH, 1, AL);
			dos(at);
			wr(R_AH, 1, 0x0c);
		}
		break;
	case 0x0e: /* select disk */
		wr(R_AL, 1, 26);
		break;
	case 0x19: /* current disk: C: */
		wr(R_AL, 1, 2);
		break;
	case 0x1a: /* set DTA */
		dta = (uint32_t)lin(DS, DX);
		break;
	case 0x25: /* set interrupt vector */
		mwr(AL * 4, 2, DX);
		mwr(AL * 4 + 2, 2, DS);
		break;
	case 0x2a: { /* get date */
		time_t t = time(NULL);
		struct tm *tm = localtime(&t);
		wr(R_CX, 2, tm->tm_year + 1900);
		wr(R_DH, 1, tm->tm_mon + 1);
		wr(R_DL, 1, tm->tm_mday);
		wr(R_AL, 1, tm->tm_wday);
		break;
	}
	case 0x2c: { /* get time */
		time_t t = time(NULL);
		struct tm *tm = localtime(&t);
		wr(R_CH, 1, tm->tm_hour);
		wr(R_CL, 1, tm->tm_min);
		wr(R_DH, 1, tm->tm_sec);
		wr(R_DL, 1, 0);
		break;
	}
	case 0x2f: /* get DTA */
		wr(R_ES, 2, dta >> 4);
		wr(R_BX, 2, dta & 0xf);
		break;
	case 0x30: /* DOS version: 5.0 */
		wr(R_AX, 2, 0x0005);
		wr(R_BX, 2, 0);
		wr(R_CX, 2, 0);
		break;
	case 0x33: /* Ctrl-Break checking: off */
		wr(R_DL, 1, 0);
		break;
	case 0x35: /* get interrupt vector */
		wr(R_BX, 2, mrd(AL * 4, 2));
		wr(R_ES, 2, mrd(AL * 4 + 2, 2));
		break;
	case 0x3b: { /* change directory */
		char dos[128], abs[256], path[1024];
		struct stat st;
		get_string(DS, DX, dos, sizeof dos);
		dos_absolute(dos, abs, sizeof abs);
		host_path(dos, path, sizeof path);
		if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode)) {
			fail_dos(3);
			break;
		}
		snprintf(cwd, sizeof cwd, "%s", abs);
		if (wc_trace) {
			fprintf(stderr, "wc: chdir \"%s\" -> \\%s\n", dos, cwd);
		}
		ok();
		break;
	}
	case 0x3c: /* create */
		open_file(O_RDWR | O_CREAT | O_TRUNC, "create");
		break;
	case 0x3d: /* open */
		open_file((AL & 3) == 0 ? O_RDONLY : (AL & 3) == 1 ? O_WRONLY : O_RDWR, "open");
		break;
	case 0x3e: { /* close */
		int fd = host_fd(BX);
		if (fd < 0) {
			fail_dos(6);
			break;
		}
		if (BX >= 5) {
			close(fd);
			handles[BX] = -1;
			if (handle_changed[BX]) {
				handle_changed[BX] = 0;
				host_file_saved(handle_path[BX]);
			}
		}
		ok();
		break;
	}
	case 0x3f: { /* read */
		int fd = host_fd(BX);
		if (fd < 0) {
			fail_dos(6);
			break;
		}
		uint64_t n = CX, base = lin(DS, DX);
		ssize_t got = read(fd, M + base, n);
		if (got < 0) {
			fail_dos(5);
			break;
		}
		wr(R_AX, 2, got);
		ok();
		break;
	}
	case 0x40: { /* write */
		int fd = host_fd(BX);
		if (fd < 0) {
			fail_dos(6);
			break;
		}
		uint64_t n = CX, base = lin(DS, DX);
		if (fd <= 2) {
			fflush(stdout);
		}
		ssize_t put = write(fd, M + base, n);
		if (put < 0) {
			fail_dos(5);
			break;
		}
		if (BX < MAX_HANDLES) {
			handle_changed[BX] = 1;
		}
		wr(R_AX, 2, put);
		ok();
		break;
	}
	case 0x41: { /* delete */
		char dos[128], path[1024];
		get_string(DS, DX, dos, sizeof dos);
		host_path(dos, path, sizeof path);
		if (unlink(path) < 0) {
			fail_dos(dos_error());
		}
		else {
			host_file_saved(path);
			ok();
		}
		break;
	}
	case 0x42: { /* seek */
		int fd = host_fd(BX);
		if (fd < 0) {
			fail_dos(6);
			break;
		}
		off_t off = (off_t)(int32_t)((CX << 16) | DX);
		off_t pos = lseek(fd, off, AL == 0 ? SEEK_SET : AL == 1 ? SEEK_CUR : SEEK_END);
		if (pos < 0) {
			fail_dos(0x19);
			break;
		}
		wr(R_AX, 2, pos & 0xffff);
		wr(R_DX, 2, (pos >> 16) & 0xffff);
		ok();
		break;
	}
	case 0x43: { /* get/set attributes */
		char dos[128], path[1024];
		struct stat st;
		get_string(DS, DX, dos, sizeof dos);
		host_path(dos, path, sizeof path);
		if (stat(path, &st) < 0) {
			fail_dos(dos_error());
			break;
		}
		wr(R_CX, 2, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
		ok();
		break;
	}
	case 0x44: /* IOCTL */
		if (AL == 0) { /* device information */
			int fd = host_fd(BX);
			if (fd < 0) {
				fail_dos(6);
				break;
			}
			wr(R_DX, 2, BX <= 4 ? 0x80d3 : 0x0002);
			ok();
		}
		else if (AL == 1) {
			ok();
		}
		else {
			fail_dos(1);
		}
		break;
	case 0x47: /* current directory */
		for (size_t i = 0; i <= strlen(cwd); i++) {
			M[lin(DS, rd(R_SI, 2) + i)] = (uint8_t)cwd[i];
		}
		ok();
		break;
	case 0x4a: /* resize memory block: always fits */
		ok();
		break;
	case 0x4c: /* exit */
		fflush(stdout);
		if (wc_trace) {
			fprintf(stderr, "wc: program exited with code %llu\n", AL);
			cpu_report(stderr);
		}
		exit((int)AL);
	case 0x4e: /* find first: nothing found */
		fail_dos(0x12);
		break;
	case 0x62: /* get PSP */
		wr(R_BX, 2, wc_psp_seg);
		break;
	default:
		fflush(stdout);
		fprintf(stderr, "wc: unimplemented DOS function INT 21h AH=%02x at %05x\n", ah, at);
		cpu_report(stderr);
		exit(2);
	}
}

static uint64_t bios_ticks(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm *tm = localtime(&ts.tv_sec);
	double secs = tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec + ts.tv_nsec / 1e9;
	return (uint64_t)(secs * 1193180.0 / 65536.0);
}

/* The BIOS and DOS services behind the vectors' initial F000:nn entries. */
void vga_bios(uint32_t at);
void mouse_int(uint32_t at);

void dos_int(uint64_t n, uint32_t at)
{
	switch (n) {
	case 0x21:
		dos(at);
		return;
	case 0x10:
		vga_bios(at);
		return;
	case 0x11: /* equipment list */
		wr(R_AX, 2, 0x0021);
		return;
	case 0x12: /* conventional memory in KB */
		wr(R_AX, 2, 640);
		return;
	case 0x16: { /* keyboard */
		uint16_t key;
		if (wc_trace) {
			fprintf(stderr, "wc: INT 16h AH=%02llx (keyboard) at %05x\n", AH, at);
		}
		switch (AH) {
		case 0x00: case 0x10: /* read */
			wr(R_AX, 2, bios_key_get(at));
			break;
		case 0x01: case 0x11: /* status: ZF clear and the key if there is one */
			if (bios_key_peek(&key)) {
				wr(R_AX, 2, key);
				wr(R_ZF, 1, 0);
			}
			else {
				wr(R_ZF, 1, 1);
			}
			break;
		case 0x02: case 0x12: /* shift flags */
			wr(R_AL, 1, M[0x417]);
			break;
		}
		return;
	}
	case 0x1a: /* timer ticks */
		if (AH == 0) {
			uint64_t t = bios_ticks();
			wr(R_CX, 2, t >> 16);
			wr(R_DX, 2, t & 0xffff);
			wr(R_AL, 1, 0);
		}
		return;
	case 0x33:
		mouse_int(at);
		return;
	default:
		fflush(stdout);
		fprintf(stderr, "wc: unimplemented interrupt INT %02llxh AX=%04llx at %05x\n", n, AX, at);
		cpu_report(stderr);
		exit(2);
	}
}
