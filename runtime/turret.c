/* Privateer: turrets that fire by themselves (WC_TURRET=0 off).
 *
 * The game fires a turret's guns only while its view (F5, F6) is on and the player aims it. Here,
 * after the player's guns each frame (a hook in FUN_5b2b_0158), each turret whose guns the player
 * switched on in its view (keys 1-4 or G there; the switch stays) now and then takes a shot
 * at the nearest hostile ship within its arc and range, through the game's own code: the turret is
 * turned towards where the ship will be (its yaw and pitch; ovl134 makes the matrix, as in its view),
 * its view is made current for the moment, and the player's gun routine (FUN_589b_0004) fires the
 * turret's guns that are on, with their energy, refire time and damage as usual. Then all is put back.
 *
 * WC_TURRET=debug reports once a second what each turret sees (its guns, the hostile ships near,
 * and why it doesn't shoot at them), and each shot; WC_TURRET_EVERY=ms between a turret's shots (1000),
 * WC_TURRET_RANGE=m (two thirds of its guns' reach: plasma flies 940 m/s for 3.4 s, so 2100 m; from
 * 3 km the shots took 3 s, and the fighters changed speed meanwhile). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wc_rt.h"

uint64_t pc_now_ns(void);
uint64_t pc_ticks(void);

enum {
	HOOK = 0x5b73c,      /* FUN_5b2b_0158 after the player's guns: [BP+6] the weapons, SI the world */
	VIEW = 0x143c,       /* DS: the view, 4 and 5 the turrets */
	TURRET1 = 0x143d,    /* DS: the turrets (0x4f bytes, ovl135), word pointers; TURRET1 + 2 the second */
	PLAYER = 0x66b4,     /* DS: the player's ship (FUN_448e_001d) */
	OBJECTS = 0x66d0,    /* DS: the world's objects, a list: node = next, object */
	FRAME_TIME = 0x2768, /* DS: the frame's time, 256 = 1 s */
};

/* A turret (ovl135): +4 its matrix (9 longs, rows right, forward, up, 256 = 1), +28h its place on
 * the ship (3 longs, 256 = 1 m), +34h yaw, +38h pitch, +3Ch..+48h their limits (lo, hi; 256 = 1 degree),
 * +4Eh fitted. A gun: +0 the shot's speed (256 = 1 m/s), +4 its life (256 = 1 s), +22h the turret it
 * is on (0 the front), +35h on. The weapons: +0Ah the ship, +0Eh the guns, +3Fh the current turret. */

static int turret_on = -1, turret_debug;
static double every_ns = 1000e6, range_m;
static uint32_t ds_base;
static uint64_t frames, next_shot[2], shots;

static uint16_t dw(uint32_t off) { return (uint16_t)mrd(ds_base + (off & 0xffff), 2); }
static int32_t dl(uint32_t off) { return (int32_t)mrd(ds_base + (off & 0xffff), 4); }

/* A far call into the game with word arguments (C order), CS:IP a runtime far pointer; DX:AX back. */
static uint32_t call_far(uint16_t seg, uint16_t off, int n, const uint16_t *args)
{
	static uint8_t saved[sizeof R];
	memcpy(saved, R, sizeof R);
	uint16_t sp = (uint16_t)rd(R_SP, 2);
	const uint32_t ss = (uint32_t)rd(R_SS, 2) * 16;
	sp -= (uint16_t)(2 * n);
	for (int i = 0; i < n; i++) mwr(ss + (uint16_t)(sp + 2 * i), 2, args[i]);
	sp -= 4;
	mwr(ss + sp, 2, 0);
	mwr(ss + (uint16_t)(sp + 2), 2, rd(R_CS, 2));
	wr(R_SP, 2, sp);
	wr(R_CS, 2, seg);
	run(((uint32_t)seg << 4) + off);
	const uint32_t r = (uint32_t)rd(R_AX, 2) | (uint32_t)rd(R_DX, 2) << 16;
	memcpy(R, saved, sizeof R);
	return r;
}

/* The same for code at a compile-time address (Ghidra's segment, 1000h-based). */
static uint32_t call_code(uint16_t gseg, uint16_t off, int n, const uint16_t *args)
{
	return call_far((uint16_t)(gseg + wc_reloc), off, n, args);
}

/* A virtual method: the vtable (in DS) holds far pointers. */
static uint32_t call_virtual(uint16_t vtable, int slot, int n, const uint16_t *args)
{
	return call_far(dw(vtable + slot + 2), dw(vtable + slot), n, args);
}

/* Whether a ship is hostile, as the radar decides (FUN_2089_078e on its pilot's faction). */
static int hostile(uint16_t obj, uint16_t player)
{
	const uint16_t a[1] = { obj };
	const uint32_t pilot = call_virtual(dw(obj), 0x30, 1, a);
	if (!pilot) return 0;
	const uint16_t b[2] = { (uint16_t)pilot, (uint16_t)(pilot >> 16) };
	const uint16_t vt = (uint16_t)mrd(lin(pilot >> 16, pilot & 0xffff), 2);
	const uint16_t faction = (uint16_t)call_virtual(vt, 8, 2, b);
	if (!faction) return 0;
	const uint16_t h[2] = { dw(faction + 0x13), player };
	return (call_code(0x2089, 0x078e, 2, h) & 0xff) != 0;
}

/* The ships' places the frame before, for the lead. */
static struct { uint16_t obj; int32_t pos[3]; int32_t prev[3]; uint64_t frame; } seen[64];

static void track(uint16_t ship_vt)
{
	for (uint16_t node = dw(OBJECTS), k = 0; node && k < 512; node = dw(node), k++) {
		const uint16_t obj = dw(node + 2);
		if (!obj || dw(obj) != ship_vt) continue;
		int i, free = -1;
		for (i = 0; i < 64 && seen[i].obj != obj; i++)
			if (free < 0 && seen[i].frame + 2 < frames) free = i;
		if (i == 64) {
			if (free < 0) continue;
			i = free;
			seen[i].obj = obj;
			seen[i].frame = 0;
		}
		memcpy(seen[i].prev, seen[i].pos, sizeof seen[i].pos);
		for (int c = 0; c < 3; c++) seen[i].pos[c] = dl(obj + 0x14 + 4 * c);
		if (seen[i].frame != frames - 1) memcpy(seen[i].prev, seen[i].pos, sizeof seen[i].pos);
		seen[i].frame = frames;
	}
}

/* A ship's velocity (m/s), 0 if not known. */
static void velocity(uint16_t obj, double v[3])
{
	const double dt = dl(FRAME_TIME) / 256.0;
	for (int i = 0; i < 64; i++)
		if (seen[i].obj == obj && seen[i].frame == frames && dt > 0) {
			for (int c = 0; c < 3; c++) v[c] = (seen[i].pos[c] - seen[i].prev[c]) / 256.0 / dt;
			return;
		}
	v[0] = v[1] = v[2] = 0;
}

static double dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* WC_TURRET=debug: each shot's closest approach to its target and how long it flew (a miss flies the
 * guns' whole life, about 105 frames for plasma; one that ends sooner hit something). */
static struct { uint16_t shot, vt, target; double min; uint64_t born; } follow[16];

static void follow_shots(void)
{
	for (int j = 0; j < 16; j++) {
		if (!follow[j].shot) continue;
		int alive = 0;
		for (uint16_t node = dw(OBJECTS), k = 0; node && k < 512; node = dw(node), k++) alive |= dw(node + 2) == follow[j].shot;
		if (alive && dw(follow[j].shot) == follow[j].vt) {
			double d = 0;
			for (int c = 0; c < 3; c++) {
				const double x = (dl(follow[j].shot + 0x14 + 4 * c) - dl(follow[j].target + 0x14 + 4 * c)) / 256.0;
				d += x * x;
			}
			if (sqrt(d) < follow[j].min) follow[j].min = sqrt(d);
			continue;
		}
		fprintf(stderr, "wc: turret: shot %04x came within %.0f m of %04x, flew %llu frames\n", follow[j].shot, follow[j].min,
			follow[j].target, (unsigned long long)(frames - follow[j].born));
		follow[j].shot = 0;
	}
}

static int follow_new(const uint16_t *before, int nb, uint16_t target)
{
	int made = 0;
	for (uint16_t node = dw(OBJECTS), k = 0; node && k < 256; node = dw(node), k++) {
		const uint16_t obj = dw(node + 2);
		int j;
		for (j = 0; j < nb && before[j] != obj; j++) {}
		if (j < nb) continue;
		made++;
		for (j = 0; j < 16 && follow[j].shot; j++) {}
		if (j == 16) continue;
		follow[j].shot = obj;
		follow[j].vt = dw(obj);
		follow[j].target = target;
		follow[j].min = 1e9;
		follow[j].born = frames;
	}
	return made;
}

/* Turret t (0, 1) shoots at the nearest hostile ship in its arc and range, if any. */
static void turret_fire(int t, uint16_t sseg, uint16_t soff, uint16_t world, uint16_t player)
{
	const uint16_t tur = dw(TURRET1 + 2 * t);
	if (!tur || !M[ds_base + (uint16_t)(tur + 0x4e)]) return;
	const uint64_t now = pc_now_ns();
	if (now < next_shot[t]) return;
	/* WC_TURRET=debug: once a second what the turret sees */
	static uint64_t status_ns[2];
	const int status = turret_debug && now >= status_ns[t];
	if (status) status_ns[t] = now + 1000000000u;

	/* Its guns, and their speed and reach from the first. */
	const uint16_t l[2] = { (uint16_t)(soff + 0xe), sseg };
	const int n = (int)(int16_t)call_code(0x592c, 0x0877, 2, l);
	uint32_t guns[16];
	int own = 0;
	double speed = 0, reach = 0;
	for (int i = 0; i < n && i < 16; i++) {
		const uint16_t g[3] = { (uint16_t)(soff + 0xe), sseg, (uint16_t)i };
		const uint32_t gun = call_code(0x592c, 0x08b6, 3, g);
		guns[i] = lin(gun >> 16, gun & 0xffff);
		/* only the guns switched on in its view (keys 1-4, G there) */
		if (M[guns[i] + 0x22] == t + 1 && M[guns[i] + 0x35] && !own++) {
			speed = (int32_t)mrd(guns[i], 4) / 256.0;
			reach = speed * (int32_t)mrd(guns[i] + 4, 4) / 256.0;
		}
	}
	if (status) {
		fprintf(stderr, "wc: turret %d: %d guns, %d of them its own and on:", t + 1, n, own);
		for (int i = 0; i < n && i < 16; i++)
			if (M[guns[i] + 0x22] == t + 1)
				fprintf(stderr, " on %d energy %d/%d", M[guns[i] + 0x35], (int32_t)mrd(guns[i] + 0x2f, 4), (int32_t)mrd(guns[i] + 0x2b, 4));
		fprintf(stderr, ", %.0f m/s, reach %.0f m (frame %llu)\n", speed, reach, (unsigned long long)frames);
	}
	if (!own || speed <= 0) return;
	const double range = range_m > 0 ? range_m : reach * 2 / 3;

	/* The ship's axes (rows) and the turret's place; one behind the ship looks back (ovl134_0000's
	 * mount 1: right and forward reversed), others forward (2, 3: top and bottom). */
	const uint16_t a[1] = { player };
	const uint16_t sm = (uint16_t)call_virtual(dw(player), 0x88, 1, a);
	double S[3][3], base[3][3], tp[3], P[3];
	for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) S[r][c] = dl(sm + 12 * r + 4 * c) / 256.0;
	for (int i = 0; i < 3; i++) tp[i] = dl(tur + 0x28 + 4 * i) / 256.0;
	for (int i = 0; i < 3; i++) P[i] = dl(player + 0x14 + 4 * i) / 256.0 + tp[0] * S[0][i] + tp[1] * S[1][i] + tp[2] * S[2][i];
	const int rear = tp[1] < 0 && -tp[1] > fabs(tp[2]);
	for (int c = 0; c < 3; c++) {
		base[0][c] = rear ? -S[0][c] : S[0][c];
		base[1][c] = rear ? -S[1][c] : S[1][c];
		base[2][c] = S[2][c];
	}
	const double yaw_lo = dl(tur + 0x3c) / 256.0, yaw_hi = dl(tur + 0x40) / 256.0;
	const double pitch_lo = dl(tur + 0x44) / 256.0, pitch_hi = dl(tur + 0x48) / 256.0;

	/* The nearest hostile ship it can turn to, aimed where it will be (the shots keep the ship's
	 * velocity, so the lead goes by the velocity relative to it). */
	double me[3], best = range, yaw = 0, pitch = 0;
	velocity(player, me);
	uint16_t target = 0;
	for (uint16_t node = dw(OBJECTS), k = 0; node && k < 512; node = dw(node), k++) {
		const uint16_t obj = dw(node + 2);
		if (!obj || obj == player || dw(obj) != dw(player)) continue;
		double d[3], v[3];
		for (int i = 0; i < 3; i++) d[i] = dl(obj + 0x14 + 4 * i) / 256.0 - P[i];
		const double dist = sqrt(dot(d, d));
		if (status && dist < 20000 && hostile(obj, player)) {
			const double lx = dot(d, base[0]), ly = dot(d, base[1]), lz = dot(d, base[2]);
			const double y = -atan2(lx, ly) * 180 / M_PI, p = atan2(lz, hypot(lx, ly)) * 180 / M_PI;
			fprintf(stderr, "wc: turret %d: hostile %04x at %.0f m, yaw %.0f pitch %.0f%s%s\n", t + 1, obj, dist, y, p,
				dist >= range ? ", out of range" : "",
				y < yaw_lo || y > yaw_hi || p < pitch_lo || p > pitch_hi ? ", out of its arc" : "");
		}
		if (dist >= best || dist < 1) continue;
		velocity(obj, v);
		for (int it = 0; it < 3; it++) {
			const double tt = sqrt(dot(d, d)) / speed;
			for (int i = 0; i < 3; i++) d[i] = dl(obj + 0x14 + 4 * i) / 256.0 - P[i] + (v[i] - me[i]) * tt;
		}
		const double lx = dot(d, base[0]), ly = dot(d, base[1]), lz = dot(d, base[2]);
		/* The game's yaw turns towards the turret's -x (checked: +30 degrees on the rear turret sent
		 * the shots towards the ship's +x), its pitch towards +z (up). */
		const double y = -atan2(lx, ly) * 180 / M_PI, p = atan2(lz, hypot(lx, ly)) * 180 / M_PI;
		if (y < yaw_lo || y > yaw_hi || p < pitch_lo || p > pitch_hi) continue;
		if (!hostile(obj, player)) continue;
		best = dist;
		yaw = y;
		pitch = p;
		target = obj;
	}
	if (!target) return;

	/* Turned there (ovl134_0000(out, turret, ship, mount) gives the base orientation, ovl134_0243
	 * (turret, those 9 longs by value) applies the yaw and pitch); its view current for the moment (FUN_589b_0004 fires the guns of the
	 * current turret that are on). */
	uint8_t saved[0x38];
	memcpy(saved, M + ds_base + (uint16_t)(tur + 4), sizeof saved);
	mwr(ds_base + (uint16_t)(tur + 0x34), 4, (uint32_t)(int32_t)lround(yaw * 256));
	mwr(ds_base + (uint16_t)(tur + 0x38), 4, (uint32_t)(int32_t)lround(pitch * 256));
	const uint16_t sp = (uint16_t)rd(R_SP, 2), out = (uint16_t)(sp - 0x80);
	const uint16_t ss = (uint16_t)rd(R_SS, 2);
	wr(R_SP, 2, (uint16_t)(sp - 0x100));
	const uint16_t b[5] = { out, ss, tur, player, (uint16_t)(rear ? 1 : 2) };
	call_code(0xd2dd, 0x0000, 5, b);
	uint16_t m[19];
	m[0] = tur;
	for (int i = 0; i < 18; i++) m[1 + i] = (uint16_t)mrd(lin(ss, (uint16_t)(out + 2 * i)), 2);
	call_code(0xd2dd, 0x0243, 19, m);
	const uint32_t sys = lin(sseg, soff);
	const uint8_t view = M[ds_base + VIEW], group = M[sys + 0x3f];
	M[ds_base + VIEW] = (uint8_t)(4 + t);
	M[sys + 0x3f] = (uint8_t)(t + 1);
	uint16_t before[256];
	int nb = 0;
	if (turret_debug)
		for (uint16_t node = dw(OBJECTS), k = 0; node && k < 256; node = dw(node), k++) before[nb++] = dw(node + 2);
	const uint16_t c[3] = { soff, sseg, world };
	call_code(0x589b, 0x0004, 3, c);
	const int made = turret_debug ? follow_new(before, nb, target) : 0;
	M[sys + 0x3f] = group;
	M[ds_base + VIEW] = view;
	memcpy(M + ds_base + (uint16_t)(tur + 4), saved, sizeof saved);
	wr(R_SP, 2, sp);
	next_shot[t] = now + (uint64_t)every_ns;
	shots++;
	if (turret_debug)
		fprintf(stderr, "wc: turret %d fires at %04x, %.0f m, yaw %.0f pitch %.0f: %d shots (frame %llu, tick %llu)\n", t + 1, target,
			best, yaw, pitch, made, (unsigned long long)frames, (unsigned long long)pc_ticks());
}

void turret_hook(uint32_t at)
{
	if (at != HOOK) return;
	if (turret_on < 0) {
		const char *s = getenv("WC_TURRET");
		turret_on = !s || strcmp(s, "0");
		turret_debug = s && !strcmp(s, "debug");
		const char *r = getenv("WC_TURRET_RANGE"), *e = getenv("WC_TURRET_EVERY");
		if (r && atof(r) > 0) range_m = atof(r);
		if (e && atof(e) > 0) every_ns = atof(e) * 1e6;
	}
	if (!turret_on || strcmp(wc_program.exe, "PRCD.EXE")) return;
	ds_base = (uint32_t)rd(R_DS, 2) * 16;
	const uint32_t bp = lin(rd(R_SS, 2), rd(R_BP, 2));
	const uint16_t soff = (uint16_t)mrd(bp + 6, 2), sseg = (uint16_t)mrd(bp + 8, 2);
	const uint16_t player = dw(PLAYER);
	if (!player || (uint16_t)mrd(lin(sseg, (uint16_t)(soff + 0xa)), 2) != player) return;
	frames++;
	track(dw(player));
	if (turret_debug) follow_shots();
	const uint8_t view = M[ds_base + VIEW];
	if (view == 4 || view == 5) return; /* the player is in a turret */
	for (int t = 0; t < 2; t++) turret_fire(t, sseg, soff, (uint16_t)rd(R_SI, 2), player);
}
