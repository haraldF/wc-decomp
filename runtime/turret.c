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
 * At exit the hits are reported (follow_shots). WC_TURRET=debug reports once a second what each turret sees (its guns, the hostile ships near,
 * and why it doesn't shoot at them), and each shot; WC_TURRET_EVERY=ms at least between a turret's
 * shots (by default it fires whenever one of its guns has recharged),
 * WC_TURRET_DRIFT=m (60: how far the target may weave off the aim while the shots fly, at its
 * present acceleration; it waits for a steadier moment), WC_TURRET_RANGE=m (1500, or two thirds of
 * its guns' reach if that is less: plasma flies 940 m/s for 3.4 s. In the user's fights, with 60 m
 * of drift, shots from under 1.5 km hit 25 of 42 times, from farther 3 of 14). */
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
static double every_ns, range_m;
static uint32_t ds_base;
static uint64_t frames, next_shot[2];

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

/* The ships' places in the last frames (and the game's time, its frame times added up): the velocity
 * from the last frame leads the aim; the acceleration, from the velocities over 4 frames now and 8 frames
 * before, tells how a ship weaves. Leading by it too (all or half of it) didn't hit more. */
enum { HISTORY = 16 };
static struct { uint16_t obj; uint64_t frame; int n; double pos[HISTORY][3], time[HISTORY]; } seen[64];
static double game_time, drift_m = 60;

static void track(uint16_t ship_vt)
{
	game_time += dl(FRAME_TIME) / 256.0;
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
			seen[i].n = 0;
		}
		if (seen[i].frame != frames - 1) seen[i].n = 0; /* a gap: start again */
		memmove(seen[i].pos[1], seen[i].pos[0], sizeof seen[i].pos[0] * (HISTORY - 1));
		memmove(seen[i].time + 1, seen[i].time, sizeof seen[i].time[0] * (HISTORY - 1));
		for (int c = 0; c < 3; c++) seen[i].pos[0][c] = dl(obj + 0x14 + 4 * c) / 256.0;
		seen[i].time[0] = game_time;
		if (seen[i].n < HISTORY) seen[i].n++;
		seen[i].frame = frames;
	}
}

/* A ship's velocity (m/s) and acceleration (m/s², at most 150), 0 if not known. */
static void motion(uint16_t obj, double v[3], double a[3])
{
	v[0] = v[1] = v[2] = a[0] = a[1] = a[2] = 0;
	for (int i = 0; i < 64; i++) {
		if (seen[i].obj != obj || seen[i].frame != frames) continue;
		const double (*p)[3] = seen[i].pos, *t = seen[i].time;
		if (seen[i].n >= 2 && t[0] > t[1])
			for (int c = 0; c < 3; c++) v[c] = (p[0][c] - p[1][c]) / (t[0] - t[1]);
		if (seen[i].n >= 13 && t[0] > t[4] && t[8] > t[12] && t[2] > t[10]) {
			double l = 0;
			for (int c = 0; c < 3; c++) {
				a[c] = ((p[0][c] - p[4][c]) / (t[0] - t[4]) - (p[8][c] - p[12][c]) / (t[8] - t[12])) / (t[2] - t[10]);
				l += a[c] * a[c];
			}
			if (sqrt(l) > 150) for (int c = 0; c < 3; c++) a[c] *= 150 / sqrt(l);
		}
		return;
	}
}

static double dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* What became of the shots, for the report at exit. A shot that hits something is removed at once;
 * one that misses flies the guns' whole life (3.4 s for plasma, a hit from 2 km ends after ~2 s). So
 * a shot that ended before 90% of its life within 150 m of a ship hit that ship (by this measure, not
 * the game's own count); a ship hit that vanished within 5 s after was destroyed. A shot's memory can
 * be reused at once by the next object: a jump of over 300 m ends it too. */
static struct { uint16_t shot, vt, target; int32_t pos[3]; double min, dist; uint64_t born, seen, life; } follow[32];
static uint64_t band_shots[5], band_hits[5]; /* by the distance fired at: < 1, 1.5, 2, 2.5 km, more */
static struct { uint16_t ship; uint64_t when; } struck[16];
static uint64_t salvos, hits, hits_target, early, missed, destroyed;
static double time_on, time_near[2], time_arc[2]; /* s in flight with turret guns on; with a hostile ship in range, in the arc too */
static uint16_t ship_vt;

static int alive_as(uint16_t obj, uint16_t vt, const uint16_t *objs, int no)
{
	for (int k = 0; k < no; k++) if (objs[k] == obj) return dw(obj) == vt;
	return 0;
}

static void follow_shots(void)
{
	uint16_t objs[512];
	int no = 0;
	for (uint16_t node = dw(OBJECTS); node && no < 512; node = dw(node)) objs[no++] = dw(node + 2);
	const uint64_t now = pc_now_ns();
	for (int j = 0; j < 32; j++) {
		if (!follow[j].shot) continue;
		int32_t pos[3];
		double moved = 0, d = 0;
		for (int c = 0; c < 3; c++) {
			pos[c] = dl(follow[j].shot + 0x14 + 4 * c);
			const double m = (pos[c] - follow[j].pos[c]) / 256.0, x = (pos[c] - dl(follow[j].target + 0x14 + 4 * c)) / 256.0;
			moved += m * m;
			d += x * x;
		}
		if (alive_as(follow[j].shot, follow[j].vt, objs, no) && moved < 300.0 * 300.0) {
			memcpy(follow[j].pos, pos, sizeof pos);
			follow[j].seen = now;
			if (sqrt(d) < follow[j].min) follow[j].min = sqrt(d);
			continue;
		}
		/* It ended: where it was last, which ship was nearest. */
		double nearest = 1e9;
		uint16_t ship = 0;
		for (int k = 0; k < no; k++) {
			if (!objs[k] || dw(objs[k]) != ship_vt || objs[k] == dw(PLAYER)) continue;
			double e = 0;
			for (int c = 0; c < 3; c++) {
				const double x = (follow[j].pos[c] - dl(objs[k] + 0x14 + 4 * c)) / 256.0;
				e += x * x;
			}
			if (sqrt(e) < nearest) nearest = sqrt(e), ship = objs[k];
		}
		const uint64_t flew = follow[j].seen - follow[j].born;
		const int ended_early = flew * 10 < follow[j].life * 9;
		const int band = follow[j].dist < 1000 ? 0 : follow[j].dist < 1500 ? 1 : follow[j].dist < 2000 ? 2 : follow[j].dist < 2500 ? 3 : 4;
		band_shots[band]++;
		if (ended_early && (nearest < 150 || follow[j].min < 150)) {
			hits++;
			band_hits[band]++;
			if (follow[j].min < 150 || ship == follow[j].target) {
				hits_target++;
				ship = follow[j].target;
			}
			/* remembered once, and only while it is there (a shot can end after its ship blew up) */
			int s, free = -1;
			for (s = 0; s < 16 && struck[s].ship != ship; s++)
				if (free < 0 && !struck[s].ship) free = s;
			if (s == 16 && free >= 0 && alive_as(ship, ship_vt, objs, no)) s = free;
			if (s < 16) struck[s].ship = ship, struck[s].when = now;
		}
		else if (ended_early) early++;
		else missed++;
		if (turret_debug)
			fprintf(stderr, "wc: turret: shot %04x came within %.0f m of %04x, flew %.2f s: %s\n", follow[j].shot, follow[j].min,
				follow[j].target, flew / 1e9, ended_early && (nearest < 150 || follow[j].min < 150) ? "hit" : ended_early ? "ended early" : "missed");
		follow[j].shot = 0;
	}
	/* Ships hit that are gone now: destroyed. */
	for (int s = 0; s < 16; s++) {
		if (!struck[s].ship) continue;
		if (!alive_as(struck[s].ship, ship_vt, objs, no)) {
			destroyed++;
			if (turret_debug) fprintf(stderr, "wc: turret: %04x destroyed\n", struck[s].ship);
			struck[s].ship = 0;
		}
		else if (now - struck[s].when > 5000000000u) struck[s].ship = 0;
	}
}

static int follow_new(const uint16_t *before, int nb, uint16_t target, double life_s, double dist)
{
	int made = 0;
	for (uint16_t node = dw(OBJECTS), k = 0; node && k < 512; node = dw(node), k++) {
		const uint16_t obj = dw(node + 2);
		int j;
		for (j = 0; j < nb && before[j] != obj; j++) {}
		if (j < nb) continue;
		made++;
		for (j = 0; j < 32 && follow[j].shot; j++) {}
		if (j == 32) continue;
		follow[j].shot = obj;
		follow[j].vt = dw(obj);
		follow[j].target = target;
		for (int c = 0; c < 3; c++) follow[j].pos[c] = dl(obj + 0x14 + 4 * c);
		follow[j].min = 1e9;
		follow[j].dist = dist;
		follow[j].born = follow[j].seen = pc_now_ns();
		follow[j].life = (uint64_t)(life_s * 1e9);
	}
	return made;
}

/* At exit: what the turrets did. */
static void turret_report(void)
{
	if (!salvos && time_on <= 0) return;
	const uint64_t ended = hits + early + missed;
	int flying = 0;
	for (int j = 0; j < 32; j++) flying += follow[j].shot != 0;
	fprintf(stderr, "wc: turrets: %llu salvos; of the shots %llu hit a ship (%llu their target), %llu ended early elsewhere, "
		"%llu missed, %d still flying: %.0f%% hit; ships destroyed after a hit: %llu\n", (unsigned long long)salvos,
		(unsigned long long)hits, (unsigned long long)hits_target, (unsigned long long)early, (unsigned long long)missed,
		flying, ended ? 100.0 * (double)hits / (double)ended : 0.0, (unsigned long long)destroyed);
	static const char *const bands[5] = { "under 1 km", "1-1.5 km", "1.5-2 km", "2-2.5 km", "farther" };
	fprintf(stderr, "wc: turrets: guns on for %.0f s in flight; a hostile ship within range for %.0f s, %.0f s of it in the arc\n",
		time_on, time_near[0] + time_near[1], time_arc[0] + time_arc[1]);
	fprintf(stderr, "wc: turrets: hits by the distance fired at:");
	for (int b = 0; b < 5; b++)
		if (band_shots[b]) fprintf(stderr, " %s %llu/%llu", bands[b], (unsigned long long)band_hits[b], (unsigned long long)band_shots[b]);
	fprintf(stderr, "\n");
}

/* Turret t (0, 1) shoots at the nearest hostile ship in its arc and range, if any. */
static void turret_fire(int t, uint16_t sseg, uint16_t soff, uint16_t world, uint16_t player)
{
	const uint16_t tur = dw(TURRET1 + 2 * t);
	if (!tur || !M[ds_base + (uint16_t)(tur + 0x4e)]) return;
	const uint64_t now = pc_now_ns();
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
	int ready = 0;
	/* The charge a shot takes is the gun's cost (+2Bh), less with what the ship's part at +6 brings when
	 * its method 28h says so: cost * 256 / its word at +32h (FUN_5b2b_0009 refills up to that, and
	 * FUN_589b_0004 fires from it; the user's Righteous Fire ship needs 104 of 128). */
	const uint16_t part = dw(player + 6);
	const uint16_t pa[1] = { part };
	const int scaled = part && (call_virtual(dw(part), 0x28, 1, pa) & 0xff);
	const uint16_t factor = scaled ? dw(part + 0x32) : 0;
	for (int i = 0; i < n && i < 16; i++) {
		const uint16_t g[3] = { (uint16_t)(soff + 0xe), sseg, (uint16_t)i };
		const uint32_t gun = call_code(0x592c, 0x08b6, 3, g);
		guns[i] = lin(gun >> 16, gun & 0xffff);
		/* only the guns switched on in its view (keys 1-4, G there) */
		if (M[guns[i] + 0x22] != t + 1 || !M[guns[i] + 0x35]) continue;
		if (!own++) {
			speed = (int32_t)mrd(guns[i], 4) / 256.0;
			reach = speed * (int32_t)mrd(guns[i] + 4, 4) / 256.0;
		}
		/* recharged: its energy (+2Fh, refilled by the frame's time) up to its cost (+2Bh, 256 = 1 s:
		 * plasma 0.72 s, tachyons 0.40 s), as FUN_589b_0004 wants it */
		const int64_t cost = (int32_t)mrd(guns[i] + 0x2b, 4);
		ready += (int32_t)mrd(guns[i] + 0x2f, 4) >= (factor ? cost * 256 / factor : cost);
	}
	if (status) {
		fprintf(stderr, "wc: turret %d: %d guns, %d of them its own and on:", t + 1, n, own);
		for (int i = 0; i < n && i < 16; i++)
			fprintf(stderr, " [%05x turret %d on %d energy %d/%d]", guns[i], M[guns[i] + 0x22], M[guns[i] + 0x35], (int32_t)mrd(guns[i] + 0x2f, 4),
				(int32_t)mrd(guns[i] + 0x2b, 4));
		fprintf(stderr, ", charge factor %d, %.0f m/s, reach %.0f m, yaw %.0f..%.0f pitch %.0f..%.0f (frame %llu)\n", factor, speed, reach,
			dl(tur + 0x3c) / 256.0, dl(tur + 0x40) / 256.0, dl(tur + 0x44) / 256.0, dl(tur + 0x48) / 256.0, (unsigned long long)frames);
	}
	if (!own || speed <= 0) return;
	const double range = range_m > 0 ? range_m : fmin(1500, reach * 2 / 3);

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
	double me[3], me_acc[3], best = range, yaw = 0, pitch = 0;
	motion(player, me, me_acc);
	uint16_t target = 0;
	int near = 0, in_arc = 0;
	for (uint16_t node = dw(OBJECTS), k = 0; node && k < 512; node = dw(node), k++) {
		const uint16_t obj = dw(node + 2);
		if (!obj || obj == player || dw(obj) != dw(player)) continue;
		double d[3], v[3], acc[3];
		for (int i = 0; i < 3; i++) d[i] = dl(obj + 0x14 + 4 * i) / 256.0 - P[i];
		const double dist = sqrt(dot(d, d));
		if (status && dist < 20000 && hostile(obj, player)) {
			const double lx = dot(d, base[0]), ly = dot(d, base[1]), lz = dot(d, base[2]);
			const double y = -atan2(lx, ly) * 180 / M_PI, p = atan2(lz, hypot(lx, ly)) * 180 / M_PI;
			double v[3], acc[3];
			motion(obj, v, acc);
			const double tt = dist / speed;
			fprintf(stderr, "wc: turret %d: hostile %04x at %.0f m, yaw %.0f pitch %.0f, speed %.0f, drift %.0f m%s%s\n", t + 1, obj, dist, y, p,
				sqrt(dot(v, v)), sqrt(dot(acc, acc)) * tt * tt / 2,
				dist >= range ? ", out of range" : "",
				y < yaw_lo || y > yaw_hi || p < pitch_lo || p > pitch_hi ? ", out of its arc" : "");
		}
		if (dist < range && dist >= 1 && hostile(obj, player)) {
			const double lx = dot(d, base[0]), ly = dot(d, base[1]), lz = dot(d, base[2]);
			const double y = -atan2(lx, ly) * 180 / M_PI, p = atan2(lz, hypot(lx, ly)) * 180 / M_PI;
			near = 1;
			if (y >= yaw_lo && y <= yaw_hi && p >= pitch_lo && p <= pitch_hi) in_arc = 1;
		}
		if (dist >= best || dist < 1) continue;
		motion(obj, v, acc);
		for (int it = 0; it < 4; it++) {
			const double tt = sqrt(dot(d, d)) / speed;
			for (int i = 0; i < 3; i++)
				d[i] = dl(obj + 0x14 + 4 * i) / 256.0 - P[i] + (v[i] - me[i]) * tt;
		}
		/* A fighter that keeps turning like this is that far off by the time the shots arrive: wait
		 * for a steadier moment. In three test fights the turrets hit with 45% of their shots without
		 * this, 54% at 100 m, 71% at 60 m (as many hits, fewer shots), 69% at 40 m, 66% at 30 m. */
		const double tt = sqrt(dot(d, d)) / speed;
		if (sqrt(dot(acc, acc)) * tt * tt / 2 > drift_m) continue;
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
	const double dt = dl(FRAME_TIME) / 256.0;
	time_near[t] += near ? dt : 0;
	time_arc[t] += in_arc ? dt : 0;
	if (!target || !ready || now < next_shot[t]) return;

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
	uint16_t before[512];
	int nb = 0;
	for (uint16_t node = dw(OBJECTS); node && nb < 512; node = dw(node)) before[nb++] = dw(node + 2);
	const uint16_t c[3] = { soff, sseg, world };
	call_code(0x589b, 0x0004, 3, c);
	const int made = follow_new(before, nb, target, reach / speed, best);
	if (made) salvos++;
	M[sys + 0x3f] = group;
	M[ds_base + VIEW] = view;
	memcpy(M + ds_base + (uint16_t)(tur + 4), saved, sizeof saved);
	wr(R_SP, 2, sp);
	next_shot[t] = now + (uint64_t)every_ns;
	if (turret_debug)
		fprintf(stderr, "wc: turret %d fires at %04x, %.0f m, yaw %.0f pitch %.0f: %d shots (frame %llu, tick %llu)\n", t + 1, target,
			best, yaw, pitch, made, (unsigned long long)frames, (unsigned long long)pc_ticks());
}

/* The game builds the ship's guns afresh, all off, at each launch (FUN_592c_010c copies them from
 * the ship's record): the turrets' switches are remembered here instead. What the player last left
 * on in a turret's view (a mask of its guns in order) is switched on again when the guns turn up all
 * off. WC_TURRET_CLEAR=frame switches every gun off at that frame (for testing). */
static uint32_t wanted[2];
static uint64_t clear_frame;

static void remember(uint16_t sseg, uint16_t soff, uint8_t view)
{
	const uint16_t l[2] = { (uint16_t)(soff + 0xe), sseg };
	const int n = (int)(int16_t)call_code(0x592c, 0x0877, 2, l);
	uint32_t guns[16];
	for (int i = 0; i < n && i < 16; i++) {
		const uint16_t g[3] = { (uint16_t)(soff + 0xe), sseg, (uint16_t)i };
		const uint32_t gun = call_code(0x592c, 0x08b6, 3, g);
		guns[i] = lin(gun >> 16, gun & 0xffff);
		if (frames == clear_frame) M[guns[i] + 0x35] = 0;
	}
	int on_now = 0;
	for (int t = 0; t < 2; t++) {
		uint32_t mask = 0, bit = 1;
		int own = 0;
		for (int i = 0; i < n && i < 16; i++)
			if (M[guns[i] + 0x22] == t + 1) {
				own++;
				if (M[guns[i] + 0x35]) mask |= bit;
				bit <<= 1;
			}
		if (!own) continue;
		if (mask || (wanted[t] && view != 4 + t)) on_now = 1;
		if (view == 4 + t) {
			wanted[t] = mask;
		} else if (!mask && wanted[t]) {
			bit = 1;
			for (int i = 0; i < n && i < 16; i++)
				if (M[guns[i] + 0x22] == t + 1) {
					if (wanted[t] & bit) M[guns[i] + 0x35] = 1;
					bit <<= 1;
				}
			if (turret_debug) fprintf(stderr, "wc: turret %d: its guns switched on again (frame %llu)\n", t + 1, (unsigned long long)frames);
		}
	}
	if (on_now) time_on += dl(FRAME_TIME) / 256.0;
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
		const char *dr = getenv("WC_TURRET_DRIFT");
		if (dr && atof(dr) > 0) drift_m = atof(dr);
		const char *cl = getenv("WC_TURRET_CLEAR");
		if (cl) clear_frame = strtoull(cl, 0, 10);
		if (turret_on) atexit(turret_report);
	}
	if (!turret_on || strcmp(wc_program.exe, "PRCD.EXE")) return;
	ds_base = (uint32_t)rd(R_DS, 2) * 16;
	const uint32_t bp = lin(rd(R_SS, 2), rd(R_BP, 2));
	const uint16_t soff = (uint16_t)mrd(bp + 6, 2), sseg = (uint16_t)mrd(bp + 8, 2);
	const uint16_t player = dw(PLAYER);
	if (!player || (uint16_t)mrd(lin(sseg, (uint16_t)(soff + 0xa)), 2) != player) return;
	frames++;
	track(dw(player));
	ship_vt = dw(player);
	follow_shots();
	const uint8_t view = M[ds_base + VIEW];
	remember(sseg, soff, view);
	if (view == 4 || view == 5) return; /* the player is in a turret */
	for (int t = 0; t < 2; t++) turret_fire(t, sseg, soff, (uint16_t)rd(R_SI, 2), player);
}
