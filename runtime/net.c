/* Two players over UDP. Both PCs run the whole game on the emulated clock (WC_CLOCK=instructions),
 * so the same inputs at the same instruction make the same game; only inputs travel.
 *   WC_NET=host:port        the wing commander: waits for the friend
 *   WC_NET=join:host:port   the friend: flies the wingman, watches everything else
 *   WC_NET_DELAY=n          input delay in blocks of 1/30 s (default 3)
 *   WC_NET_WATCH=1          the friend starts by watching the AI fly the wingman; W switches, in flight too
 *   WC_NET_LOSS=percent     drop this share of the packets sent, for testing
 *   WC_NET_LOG=1            print the machine's hash once a second, to compare two runs
 *   WC_NET_MUTE=1           no sound from this one, with both on one PC
 *   WC_NET_PREDICT=0        the friend's view doesn't run ahead of the game (see pc.c)
 * Time is cut into blocks of 1/30 s of emulated time. What a player does during block B is carried
 * out in block B + delay, on both PCs, and neither enters a block before it holds the other's
 * inputs for it. Packets are unreliable and repeat everything the other side hasn't acknowledged,
 * so a lost one costs nothing but the wait for the next. The game (and a save) must be the same on
 * both sides, which the handshake checks. */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "host.h"
#include "net.h"
#include "wc_rt.h"

enum {
	BLOCK_NS = 33333333,
	RING = 512,            /* blocks kept */
	MAX_EVENTS = 12,       /* per block; more wait for the next one */
	MAX_PACKET = 1200,
	HASH_EVERY = 30,       /* blocks */
	SEND_NS = 5000000,     /* repeat at most this often */
	PEER_GONE_NS = 5000000000u,
	MAGIC = 0x314e4357,    /* "WCN1" */
};

enum { P_HELLO = 1, P_WELCOME, P_BLOCKS };
enum { E_KEY = 1, E_MOUSE, E_MOUSE_MOVE, E_JOYSTICK };

struct event { uint8_t type, code; int16_t a, b, c; };

struct block {
	uint32_t number;       /* valid when it matches the slot's block */
	int valid;
	int8_t x, y, roll, throttle;
	uint8_t fire, count;
	uint32_t hash_block;   /* which block hash is of (0: none) */
	uint64_t hash;
	struct event events[MAX_EVENTS];
};

int wc_net_on, wc_net_join;
static int sock, delay_blocks = 3, loss, net_log;
static struct sockaddr_storage peer;
static socklen_t peer_len;
static int have_peer, peer_gone;
static struct block local[RING], remote[RING];
static uint32_t next_block, local_top; /* the next block to enter; local blocks exist up to here */
static uint32_t peer_next;             /* the next block we hold nothing of */
static uint32_t peer_acked;            /* the peer holds our blocks below this */
static uint64_t next_block_ns, last_send_ns, last_heard_ns;
static time_t time_base;
static uint64_t hashes[RING];
static int desyncs;

static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;
static struct event pending[256];
static int pending_count;
static _Thread_local int applying;

/* the friend's controls: what they hold now, and what is in force in this block */
static uint8_t held[128];
static int stick_x, stick_y, stick_roll, stick_fire;
static int f_x, f_y, f_roll, f_throttle, f_fire, f_watch, f_burn;
static int watch_wanted;
static int ramp[3]; /* the arrow keys' x, y, roll: as the game's own keys (FUN_13be_0945) */
static uint8_t view_keys[16], view_down[128]; /* T and L, for the friend's view alone */
static int view_keys_count;
static struct block ahead_now; /* the controls as they are this moment, not in a block yet (net_ahead) */
static int ahead_saving;

static uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint64_t fnv(uint64_t h, const void *data, size_t n)
{
	const uint8_t *p = data;
	while (n--) h = (h ^ *p++) * 0x100000001b3u;
	return h;
}

/* The game, the save games and the settings that change what the game does. */
static uint64_t setup_hash(void)
{
	extern const char *wc_game_dir;
	static const char *const env[] = { "WC_CYCLES", "WC_FPS", "WC_SB", "WC_EMS", "WC_MT32", "WC_JOYSTICK", "WC_LOAD_SEG" };
	const char *files[] = { wc_program.exe, "WINGCMDR.CFG", "GAMEDAT/SAVEGAME.WLD" };
	uint64_t h = 0xcbf29ce484222325u;
	for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
		char path[1100], buf[65536];
		snprintf(path, sizeof path, "%s/%s", wc_game_dir, files[i]);
		FILE *f = fopen(path, "rb");
		size_t n;
		while (f && (n = fread(buf, 1, sizeof buf, f)) > 0) h = fnv(h, buf, n);
		if (f) fclose(f);
	}
	for (size_t i = 0; i < sizeof env / sizeof *env; i++) {
		const char *e = getenv(env[i]);
		h = fnv(h, e ? e : "-", strlen(e ? e : "-") + 1);
	}
	return h;
}

static void fail(const char *what)
{
	fprintf(stderr, "wc: net: %s\n", what);
	host_exit(6);
}

void net_init(void)
{
	const char *spec = getenv("WC_NET");
	if (!spec) return;
	wc_net_on = 1;
	setenv("WC_CLOCK", "instructions", 1);
	const char *d = getenv("WC_NET_DELAY");
	if (d) delay_blocks = atoi(d);
	if (delay_blocks < 1) delay_blocks = 1;
	if (delay_blocks > 60) delay_blocks = 60;
	const char *l = getenv("WC_NET_LOSS");
	if (l) loss = atoi(l);
	net_log = getenv("WC_NET_LOG") != NULL;
	watch_wanted = getenv("WC_NET_WATCH") != NULL;

	char host[256] = "";
	int port = 0;
	if (sscanf(spec, "host:%d", &port) == 1) wc_net_join = 0;
	else if (sscanf(spec, "join:%255[^:]:%d", host, &port) == 2) wc_net_join = 1;
	else fail("WC_NET must be host:port or join:host:port");

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) fail("no socket");
	fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
	if (wc_net_join) {
		struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res;
		char service[16];
		snprintf(service, sizeof service, "%d", port);
		if (getaddrinfo(host, service, &hints, &res) || !res) fail("can't find the host");
		memcpy(&peer, res->ai_addr, res->ai_addrlen);
		peer_len = res->ai_addrlen;
		freeaddrinfo(res);
		have_peer = 1;
	} else {
		struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY) };
		if (bind(sock, (struct sockaddr *)&a, sizeof a) < 0) fail("can't use that port");
	}
}

static void send_packet(const void *data, size_t n)
{
	if (!have_peer || (loss && (int)(arc4random() % 100) < loss)) return;
	sendto(sock, data, n, 0, (struct sockaddr *)&peer, peer_len);
}

static void send_small(uint8_t type, uint64_t a, uint64_t b)
{
	uint8_t p[21];
	uint32_t magic = MAGIC;
	memcpy(p, &magic, 4);
	p[4] = type;
	memcpy(p + 5, &a, 8);
	memcpy(p + 13, &b, 8);
	send_packet(p, sizeof p);
}

/* Handshake: the friend says hello with their setup's hash; the host answers with the time. */
void net_connect(void)
{
	if (!wc_net_on) return;
	const uint64_t mine = setup_hash();
	uint8_t p[MAX_PACKET];
	uint64_t last = 0;
	fprintf(stderr, wc_net_join ? "wc: net: joining\n" : "wc: net: waiting for your friend\n");
	for (;;) {
		struct sockaddr_storage from;
		socklen_t from_len = sizeof from;
		ssize_t n = recvfrom(sock, p, sizeof p, 0, (struct sockaddr *)&from, &from_len);
		uint32_t magic = 0;
		if (n >= 21) memcpy(&magic, p, 4);
		if (magic == MAGIC) {
			uint64_t a, b;
			memcpy(&a, p + 5, 8);
			memcpy(&b, p + 13, 8);
			if (!wc_net_join && p[4] == P_HELLO) {
				memcpy(&peer, &from, from_len);
				peer_len = from_len;
				have_peer = 1;
				if (!time_base) time_base = time(NULL);
				send_small(P_WELCOME, (uint64_t)time_base, mine);
				if (a != mine) fail("your friend's game, save or settings differ from yours");
				break;
			}
			if (wc_net_join && p[4] == P_WELCOME) {
				if (b != mine) fail("the host's game, save or settings differ from yours");
				time_base = (time_t)a;
				break;
			}
		}
		uint64_t now = mono_ns();
		if (wc_net_join && now - last > 100000000u) {
			send_small(P_HELLO, mine, 0);
			last = now;
		}
		usleep(1000);
	}
	fprintf(stderr, "wc: net: connected, %s, input delay %d ms\n", wc_net_join ? "you fly the wingman" : "you are the commander",
		delay_blocks * 1000 / 30);
	last_heard_ns = mono_ns();
	for (uint32_t b = 0; b < (uint32_t)delay_blocks; b++) { /* nobody can have done anything by then */
		local[b].number = b;
		local[b].valid = 1;
		remote[b].number = b;
		remote[b].valid = 1;
	}
	local_top = peer_next = peer_acked = (uint32_t)delay_blocks;
}

static void send_blocks(void)
{
	uint8_t p[MAX_PACKET];
	uint32_t magic = MAGIC;
	memcpy(p, &magic, 4);
	p[4] = P_BLOCKS;
	memcpy(p + 5, &peer_next, 4); /* acknowledges everything below */
	size_t at = 11;
	uint16_t count = 0;
	for (uint32_t b = peer_acked; b < local_top; b++) {
		const struct block *s = &local[b % RING];
		size_t size = offsetof(struct block, events) + s->count * sizeof(struct event);
		if (at + size > sizeof p) break;
		memcpy(p + at, s, size);
		at += size;
		count++;
	}
	memcpy(p + 9, &count, 2);
	send_packet(p, at);
}

static uint32_t pending_hash_block[RING];
static uint64_t pending_hash[RING];

static void receive(void)
{
	uint8_t p[MAX_PACKET];
	ssize_t n;
	while ((n = recvfrom(sock, p, sizeof p, 0, NULL, NULL)) >= 11) {
		uint32_t magic;
		memcpy(&magic, p, 4);
		if (magic != MAGIC) continue;
		if (p[4] == P_HELLO) { /* our welcome got lost: it is sent again */
			send_small(P_WELCOME, (uint64_t)time_base, setup_hash());
			continue;
		}
		if (p[4] != P_BLOCKS) continue;
		last_heard_ns = mono_ns();
		uint32_t acked;
		uint16_t count;
		memcpy(&acked, p + 5, 4);
		memcpy(&count, p + 9, 2);
		if (acked > peer_acked && acked <= local_top) peer_acked = acked;
		size_t at = 11;
		for (uint16_t i = 0; i < count; i++) {
			struct block s;
			size_t head = offsetof(struct block, events);
			if (at + head > (size_t)n) break;
			memcpy(&s, p + at, head);
			size_t size = head + s.count * sizeof(struct event);
			if (s.count > MAX_EVENTS || at + size > (size_t)n) break;
			if (s.number == peer_next) {
				struct block *d = &remote[s.number % RING];
				memcpy(d, p + at, size);
				d->valid = 1;
				peer_next++;
				if (s.hash_block) {
					pending_hash_block[s.hash_block % RING] = s.hash_block;
					pending_hash[s.hash_block % RING] = s.hash;
				}
			}
			at += size;
		}
	}
}

static void pump(void)
{
	uint64_t now = mono_ns();
	receive();
	if (now - last_send_ns >= SEND_NS) {
		last_send_ns = now;
		send_blocks();
	}
}

static int clamp8(int v)
{
	return v < -8 ? -8 : v > 8 ? 8 : v;
}

static void ramp_step(int axis, int dir)
{
	if (held[0x2a] || held[0x36]) ramp[axis] = dir * 8; /* with shift: at once */
	else if (ramp[axis] * dir < 0) ramp[axis] = 0; /* the other way: stops first */
	else if (ramp[axis] * dir < 8) ramp[axis] += dir;
}

/* Each key event, so also each repeat, is a step; a tap is a small turn. */
static void ramp_key(uint8_t code)
{
	switch (code) {
	case 0x47: ramp_step(1, 1); ramp_step(0, -1); break;
	case 0x48: ramp_step(1, 1); break;
	case 0x49: ramp_step(1, 1); ramp_step(0, 1); break;
	case 0x4b: ramp_step(0, -1); break;
	case 0x4d: ramp_step(0, 1); break;
	case 0x4f: ramp_step(1, -1); ramp_step(0, -1); break;
	case 0x50: ramp_step(1, -1); break;
	case 0x51: ramp_step(1, -1); ramp_step(0, 1); break;
	case 0x33: case 0x52: ramp_step(2, -1); break;
	case 0x34: case 0x53: ramp_step(2, 1); break;
	}
	if (!(held[0x4b] || held[0x4d] || held[0x47] || held[0x49] || held[0x4f] || held[0x51])) ramp[0] = 0;
	if (!(held[0x48] || held[0x50] || held[0x47] || held[0x49] || held[0x4f] || held[0x51])) ramp[1] = 0;
	if (!(held[0x33] || held[0x34] || held[0x52] || held[0x53])) ramp[2] = 0;
}

/* The friend's input: their keys and stick become the wingman's controls. */
static void friend_event(const struct event *e)
{
	switch (e->type) {
	case E_KEY:
		if (e->code == 0x11 && !held[0x11]) { /* W: the AI flies, or they do */
			watch_wanted = !watch_wanted;
			if (!ahead_saving) fprintf(stderr, "wc: net: %s\n", watch_wanted ? "watching the wingman fly" : "you fly the wingman");
		}
		held[e->code & 0x7f] = !(e->code & 0x80);
		ramp_key(e->code & 0x80 ? 0 : e->code);
		break;
	case E_JOYSTICK:
		stick_x = e->a * 8 / 1000;
		stick_y = -e->b * 8 / 1000; /* stick forward is the up arrow */
		stick_roll = e->c * 8 / 1000;
		stick_fire = e->code & 1;
		break;
	}
}

static void friend_controls(struct block *s)
{
	s->x = (int8_t)clamp8(ramp[0] ? ramp[0] : stick_x);
	s->y = (int8_t)clamp8(ramp[1] ? ramp[1] : stick_y);
	s->roll = (int8_t)clamp8(ramp[2] ? ramp[2] : stick_roll);
	s->throttle = (int8_t)(held[0x0d] ? 1 : held[0x0c] ? -1 : 0);
	s->fire = (held[0x39] || held[0x1c] || stick_fire) | (watch_wanted ? 2 : 0) | (held[0x0f] ? 4 : 0);
}

static void commit_local(uint32_t number, uint64_t hash, uint32_t hash_block)
{
	struct block *s = &local[number % RING];
	memset(s, 0, sizeof *s);
	s->number = number;
	s->valid = 1;
	s->hash = hash;
	s->hash_block = hash_block;
	pthread_mutex_lock(&pending_lock);
	int used = 0;
	if (wc_net_join) {
		for (; used < pending_count; used++) friend_event(&pending[used]);
	} else {
		for (; used < pending_count && s->count < MAX_EVENTS; used++) s->events[s->count++] = pending[used];
	}
	memmove(pending, pending + used, (size_t)(pending_count - used) * sizeof *pending);
	pending_count -= used;
	pthread_mutex_unlock(&pending_lock);
	if (wc_net_join) friend_controls(s);
	local_top = number + 1;
}

static void apply(const struct block *commander, const struct block *friend)
{
	applying = 1;
	for (int i = 0; i < commander->count; i++) {
		const struct event *e = &commander->events[i];
		switch (e->type) {
		case E_KEY: pc_net_key(e->code); break;
		case E_MOUSE: pc_mouse(e->a, e->b, e->code); break;
		case E_MOUSE_MOVE: pc_mouse_move(e->a, e->b, e->code); break;
		case E_JOYSTICK: pc_joystick(e->a, e->b, e->c, e->code); break;
		}
	}
	applying = 0;
	f_x = friend->x;
	f_y = friend->y;
	f_roll = friend->roll;
	f_throttle = friend->throttle;
	f_fire = friend->fire & 1;
	f_burn = (friend->fire & 4) != 0;
	f_watch = (friend->fire & 2) != 0; /* in the friend's block, so at the same moment on both PCs */
}

/* The machine as both PCs must have it. Video memory is left out: the friend's isn't shown anyway. */
static uint64_t machine_hash(void)
{
	return fnv(fnv(0xcbf29ce484222325u, M, 0xa0000), R, sizeof R);
}

void net_poll(void)
{
	if (!wc_net_on) return;
	uint64_t now = pc_now_ns();
	if (now < next_block_ns) {
		static uint64_t last_pump;
		uint64_t real = mono_ns();
		if (real - last_pump >= 2000000u) {
			last_pump = real;
			pump();
		}
		return;
	}
	while (now >= next_block_ns) {
		const uint32_t b = next_block;
		/* Wait for the friend's or the commander's inputs for this block. */
		while (!peer_gone && remote[b % RING].number != b) {
			pump();
			if (remote[b % RING].number == b && remote[b % RING].valid) break;
			if (mono_ns() - last_heard_ns > PEER_GONE_NS) {
				if (wc_net_join) fail("the commander has gone");
				fprintf(stderr, "wc: net: your friend has gone; the AI flies the wingman again\n");
				peer_gone = 1;
				break;
			}
			usleep(500);
		}
		uint64_t hash = 0;
		uint32_t hash_block = 0;
		if (b && !(b % HASH_EVERY)) {
			hash = machine_hash();
			hash_block = b;
			hashes[b % RING] = hash;
			if (net_log) fprintf(stderr, "wc: net: block %u hash %016llx\n", b, (unsigned long long)hash);
		}
		/* Compare with theirs, which may have come before or after ours. */
		for (uint32_t h = b / HASH_EVERY * HASH_EVERY, i = 0; h && i < RING / HASH_EVERY - 1; i++, h -= HASH_EVERY) {
			if (pending_hash_block[h % RING] != h) continue;
			if (hashes[h % RING] != pending_hash[h % RING] && !desyncs++)
				fprintf(stderr, "wc: net: the two games differ from block %u on\n", h);
			if (net_log) fprintf(stderr, "wc: net: block %u compared\n", h);
			pending_hash_block[h % RING] = 0;
			if (h < HASH_EVERY) break;
		}
		const struct block empty = { 0 };
		const struct block *mine = &local[b % RING], *theirs = peer_gone ? &empty : &remote[b % RING];
		apply(wc_net_join ? theirs : mine, wc_net_join ? mine : theirs);
		commit_local(b + (uint32_t)delay_blocks, hash, hash_block);
		next_block = b + 1;
		next_block_ns += BLOCK_NS;
	}
	pump();
}

void net_local_key(uint8_t code)
{
	if (getenv("WC_VIEW_PRED")) fprintf(stderr, "wc: pred key real %llu code %x\n", (unsigned long long)(mono_ns() / 1000), code);
	pthread_mutex_lock(&pending_lock);
	if (pending_count < 256) pending[pending_count++] = (struct event){ .type = E_KEY, .code = code };
	const uint8_t key = code & 0x7f;
	if (wc_net_join && (key == 0x14 || key == 0x26)) {
		if (!(code & 0x80) && !view_down[key] && view_keys_count < (int)sizeof view_keys) view_keys[view_keys_count++] = key;
		view_down[key] = !(code & 0x80);
	}
	pthread_mutex_unlock(&pending_lock);
}

static void local_event(struct event e, int merge)
{
	pthread_mutex_lock(&pending_lock);
	struct event *last = pending_count ? &pending[pending_count - 1] : NULL;
	if (merge && last && last->type == e.type && last->code == e.code) {
		if (e.type == E_MOUSE_MOVE) {
			last->a += e.a;
			last->b += e.b;
		} else {
			*last = e;
		}
	} else if (pending_count < 256) {
		pending[pending_count++] = e;
	}
	pthread_mutex_unlock(&pending_lock);
}

int net_capture_mouse(int relative, int x, int y, int buttons)
{
	if (!wc_net_on || applying) return 0;
	local_event((struct event){ .type = relative ? E_MOUSE_MOVE : E_MOUSE, .code = (uint8_t)buttons, .a = (int16_t)x, .b = (int16_t)y }, 1);
	return 1;
}

int net_capture_joystick(int x, int y, int roll, int buttons)
{
	if (!wc_net_on || applying) return 0;
	local_event((struct event){ .type = E_JOYSTICK, .code = (uint8_t)buttons, .a = (int16_t)x, .b = (int16_t)y, .c = (int16_t)roll }, 1);
	return 1;
}

int net_friend(int *x, int *y, int *roll, int *throttle, int *fire)
{
	if (!wc_net_on || peer_gone || f_watch) return 0;
	*x = f_x;
	*y = f_y;
	*roll = f_roll;
	*throttle = f_throttle;
	*fire = f_fire;
	return 1;
}

/* Before the fork: another thread may hold the lock then, for good in the child. */
void net_ahead_prepare(void)
{
	uint8_t was_held[sizeof held];
	int was_ramp[3];
	memcpy(was_ramp, ramp, sizeof ramp);
	const int x = stick_x, y = stick_y, roll = stick_roll, fire = stick_fire, watch = watch_wanted;
	memcpy(was_held, held, sizeof held);
	pthread_mutex_lock(&pending_lock);
	ahead_saving = 1;
	for (int i = 0; i < pending_count; i++) friend_event(&pending[i]);
	ahead_saving = 0;
	pthread_mutex_unlock(&pending_lock);
	friend_controls(&ahead_now);
	memcpy(held, was_held, sizeof held);
	memcpy(ramp, was_ramp, sizeof ramp);
	stick_x = x;
	stick_y = y;
	stick_roll = roll;
	stick_fire = fire;
	watch_wanted = watch;
}

int net_view_key(void)
{
	pthread_mutex_lock(&pending_lock);
	int key = 0;
	if (view_keys_count) {
		key = view_keys[0];
		memmove(view_keys, view_keys + 1, (size_t)--view_keys_count);
	}
	pthread_mutex_unlock(&pending_lock);
	return key;
}

int net_friend_burn(void)
{
	return f_burn;
}

int net_ahead(uint64_t at, int *x, int *y, int *roll, int *throttle, int *fire, int *burn)
{
	const uint32_t b = next_block - 1 + (at >= next_block_ns ? (uint32_t)((at - next_block_ns) / BLOCK_NS) + 1 : 0);
	const struct block *s = b < local_top ? &local[b % RING] : &ahead_now;
	*x = s->x;
	*y = s->y;
	*roll = s->roll;
	*throttle = s->throttle;
	*fire = s->fire & 1;
	*burn = (s->fire & 4) != 0;
	return b >= local_top;
}

time_t net_time(void)
{
	return wc_net_on ? time_base + (time_t)(pc_now_ns() / 1000000000u) : time(NULL);
}
