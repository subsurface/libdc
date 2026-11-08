// AI-generated (Claude)
#include <stdio.h>

#include "shearwater_tank_gas.h"

#define NONE SHEARWATER_TANK_GAS_NONE
#define NTANKS 4
#define NSAMPLES 60
#define MINUSE 70

typedef struct {
	unsigned int gas[NSAMPLES];
	unsigned int pressure[NSAMPLES * NTANKS];
} trace_t;

// Breathe gas mix 'gas' from sample 'from' up to (not including) sample 'to'.
static void
breathe (trace_t *t, unsigned int from, unsigned int to, unsigned int gas)
{
	for (unsigned int n = from; n < to; ++n)
		t->gas[n] = gas;
}

// Tank 'tank' reads 'start' at sample 'from', falling by 'step' per sample.
static void
reading (trace_t *t, unsigned int tank, unsigned int from, unsigned int to, unsigned int start, int step)
{
	for (unsigned int n = from; n < to; ++n)
		t->pressure[n * NTANKS + tank] = start - step * (int) (n - from);
}

static int
check (const char *name, const trace_t *t, unsigned int sidemount, const unsigned int expected[NTANKS])
{
	unsigned int result[NTANKS];
	shearwater_tank_gas_assign (t->gas, t->pressure, NSAMPLES, NTANKS, 3, sidemount, MINUSE, result);
	for (unsigned int i = 0; i < NTANKS; ++i) {
		if (result[i] != expected[i]) {
			fprintf (stderr, "FAIL: %s: tank %u got %d, expected %d\n", name, i, (int) result[i], (int) expected[i]);
			return 1;
		}
	}
	return 0;
}

int
main (void)
{
	int failures = 0;

	// Sidemount pair on bottom gas (2), stages on 50% (1) and oxygen (0)
	// with their valves closed until the switch: before that, the stage
	// transmitters read the pressure in their regulators bleeding down.
	trace_t a = {0};
	breathe (&a, 0, 30, 2);
	breathe (&a, 30, 45, 1);
	breathe (&a, 45, 60, 0);
	reading (&a, 0, 0, 30, 1500, 15);
	reading (&a, 0, 30, 60, 1065, 0);
	reading (&a, 1, 0, 30, 1550, 15);
	reading (&a, 1, 30, 60, 1115, 0);
	reading (&a, 2, 0, 30, 1000, 20);
	reading (&a, 2, 30, 45, 1400, 15);
	reading (&a, 2, 45, 60, 1190, 0);
	reading (&a, 3, 0, 45, 900, 15);
	reading (&a, 3, 45, 60, 1100, 15);
	const unsigned int expected_a[NTANKS] = {2, 2, 1, 0};
	failures += check ("sidemount with closed stages", &a, 0x3, expected_a);

	// Without the sidemount flag, each tank still gets the gas it was used on.
	failures += check ("doubles without sidemount flag", &a, 0, expected_a);

	// Cave staging: sidemount pair and two staged tanks, all on air (0).
	trace_t b = {0};
	breathe (&b, 0, 60, 0);
	reading (&b, 0, 0, 20, 1500, 10);
	reading (&b, 0, 20, 60, 1310, 0);
	reading (&b, 1, 0, 20, 1500, 0);
	reading (&b, 1, 20, 60, 1500, 10);
	reading (&b, 2, 0, 40, 1400, 0);
	reading (&b, 2, 40, 60, 1400, 10);
	reading (&b, 3, 0, 60, 1400, 3);
	const unsigned int expected_b[NTANKS] = {0, 0, 0, 0};
	failures += check ("all tanks on one gas", &b, 0x3, expected_b);

	// A stage that was carried but never breathed, and a tank without data.
	trace_t c = {0};
	breathe (&c, 0, 40, 2);
	breathe (&c, 40, 60, 1);
	reading (&c, 0, 0, 60, 1500, 10);
	reading (&c, 1, 0, 60, 1400, 0);
	const unsigned int expected_c[NTANKS] = {2, NONE, NONE, NONE};
	failures += check ("unused stage", &c, 0, expected_c);

	// Used about as much on two gas mixes (a switch logged late): no guess.
	trace_t d = {0};
	breathe (&d, 0, 30, 1);
	breathe (&d, 30, 60, 0);
	reading (&d, 0, 0, 60, 1200, 5);
	const unsigned int expected_d[NTANKS] = {NONE, NONE, NONE, NONE};
	failures += check ("ambiguous", &d, 0, expected_d);

	// One side of a sidemount pair barely breathed: it goes with its pair.
	trace_t e = {0};
	breathe (&e, 0, 60, 2);
	reading (&e, 0, 0, 60, 1500, 15);
	reading (&e, 1, 0, 60, 1500, 0);
	reading (&e, 1, 50, 60, 1500, 1);
	const unsigned int expected_e[NTANKS] = {2, 2, NONE, NONE};
	failures += check ("one-sided sidemount", &e, 0x3, expected_e);
	failures += check ("one-sided without sidemount flag", &e, 0, (const unsigned int[NTANKS]) {2, NONE, NONE, NONE});

	return failures ? 1 : 0;
}
