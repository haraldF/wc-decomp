/* The game port (201h) with joystick A, fed from the host's game controller. On a PC, writing the
 * port starts four one-shots whose time depends on each axis' resistance; a bit reads 1 until its
 * one-shot ends. Wing Commander measures an axis by counting how many of N reads find its bit
 * still 1 (FUN_198a_007c), finds N itself at startup by raising it until the centred stick fits
 * (FUN_1943_0003), and takes the centre from there. So here an axis bit stays 1 for a number of
 * reads, not a time: the centre is CENTRE reads, full deflection RANGE more or fewer, whatever the
 * CPU speed. Joystick B is absent (its bits never fall), the buttons read 0 while pressed.
 * The stick is always there, centred and still unless a controller moves it: the game only acts on
 * a stick that is off centre or a button that changes, and it detects the stick once, at startup,
 * so a controller plugged in later works too. WC_JOYSTICK=0: no joystick.
 * A twist (roll) has no place on the game port: in flight, it sets the roll rate (like the Ins and
 * Del keys, -9..9) right where the game turns its rates into turning speeds (wc_hook). */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "wc_rt.h"

enum { CENTRE = 256, RANGE = 240 };

static _Atomic int stick_x, stick_y, stick_roll, stick_buttons; /* -1000..1000, bit 0/1: buttons 1/2 */
static int present = -1;
static int reads, left_x, left_y; /* reads since the last write; one-shot lengths in reads */
static int rolling; /* the twist set the roll rate last time */

static int clamp(int v)
{
	return v < -1000 ? -1000 : v > 1000 ? 1000 : v;
}

void pc_joystick(int x, int y, int roll, int buttons)
{
	atomic_store(&stick_x, clamp(x));
	atomic_store(&stick_y, clamp(y));
	atomic_store(&stick_roll, clamp(roll));
	atomic_store(&stick_buttons, buttons);
}

static int is_present(void)
{
	if (present < 0) {
		const char *e = getenv("WC_JOYSTICK");
		present = !(e && !strcmp(e, "0"));
	}
	return present;
}

void joystick_out(void)
{
	reads = 0;
	left_x = CENTRE + atomic_load(&stick_x) * RANGE / 1000;
	left_y = CENTRE + atomic_load(&stick_y) * RANGE / 1000;
}

uint8_t joystick_in(void)
{
	if (!is_present()) return 0xff; /* axes never time out, buttons up */
	uint8_t v = 0xfc; /* joystick B's axes and buttons: absent */
	if (reads < left_x) v |= 1;
	if (reads < left_y) v |= 2;
	if (reads < 0x7fff) reads++;
	int b = atomic_load(&stick_buttons);
	if (b & 1) v &= (uint8_t)~0x10;
	if (b & 2) v &= (uint8_t)~0x20;
	return v;
}

/* In flight, before the rates become turning speeds: a twisted stick sets the roll rate at DS:var,
 * 1..9 either way (any twist rolls); untwisting stops the roll once, the keys roll as before. */
void joystick_roll(uint16_t var)
{
	int r = atomic_load(&stick_roll);
	if (!is_present() || (!r && !rolling)) return;
	rolling = r != 0;
	int rate = r > 0 ? (r * 9 + 999) / 1000 : -((-r * 9 + 999) / 1000);
	mwr(lin(rd(R_DS, 2), var), 2, (uint16_t)rate);
}
