#include <stdio.h>

#include "mares_iconhd_freedive.h"

static int
check (const char *name, int expected, unsigned int model, const unsigned char *header,
	unsigned int headersize, unsigned int index, unsigned int subindex,
	const unsigned char *response, unsigned int length)
{
	int actual = mares_iconhd_freedive_unavailable (model, header, headersize,
		index, subindex, response, length);
	if (actual != expected) {
		fprintf (stderr, "FAIL: %s\n", name);
		return 1;
	}
	return 0;
}

int
main (void)
{
	unsigned char header[200] = {1, 0};
	unsigned char response[16] = {0x80, 0x1E, 0x30, 0x03, 0x20, 0, 0, 8};
	int failures = 0;

	header[0x0C] = 0x25; // Settings: freedive mode 5.
	failures += check ("Sirius freedive", 1, SIRIUS, header, sizeof (header), 0x301E, 3, response, sizeof (response));
	response[1] = 0x28; // The other reported Sirius index.
	failures += check ("second Sirius freedive", 1, SIRIUS, header, sizeof (header), 0x3028, 3, response, sizeof (response));
	failures += check ("wrong index", 0, SIRIUS, header, sizeof (header), 0x301E, 3, response, sizeof (response));
	failures += check ("other model", 0, 0x1C, header, sizeof (header), 0x3028, 3, response, sizeof (response));
	failures += check ("short header", 0, SIRIUS, header, 15, 0x3028, 3, response, sizeof (response));
	failures += check ("wrong subindex", 0, SIRIUS, header, sizeof (header), 0x3028, 2, response, sizeof (response));
	failures += check ("truncated response", 0, SIRIUS, header, sizeof (header), 0x3028, 3, response, 15);
	response[7] = 9;
	failures += check ("different error response", 0, SIRIUS, header, sizeof (header), 0x3028, 3, response, sizeof (response));
	response[7] = 8;
	header[0x0C] = 0x20;
	failures += check ("scuba mode", 0, SIRIUS, header, sizeof (header), 0x3028, 3, response, sizeof (response));
	header[0x0C] = 0x25;
	header[0] = 2;
	failures += check ("unknown header type", 0, SIRIUS, header, sizeof (header), 0x3028, 3, response, sizeof (response));
	return failures != 0;
}
