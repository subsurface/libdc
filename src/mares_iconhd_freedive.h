/* Internal matching of the Sirius freedive data response. */
#ifndef MARES_ICONHD_FREEDIVE_H
#define MARES_ICONHD_FREEDIVE_H

#include <string.h>
#include "array.h"

#define SIRIUS 0x2F
#define OBJ_DIVE 0x3000
#define OBJ_DIVE_DATA 0x03

static int
mares_iconhd_freedive_unavailable (unsigned int model, const unsigned char *header,
	unsigned int headersize, unsigned int index, unsigned int subindex,
	const unsigned char *response, unsigned int length)
{
	static const unsigned char suffix[] = {0x20, 0x00, 0x00, 0x08, 0, 0, 0, 0, 0, 0, 0, 0};

	// Two Sirius logs show this response for the data of a freedive (mode 5).
	// Do not skip other modes, models or unexpected responses.
	return model == SIRIUS && subindex == OBJ_DIVE_DATA && index >= OBJ_DIVE &&
		headersize >= 0x10 && array_uint16_le (header) == 1 &&
		(array_uint32_le (header + 0x0C) & 0x0F) == 5 &&
		length == 16 && response[0] == 0x80 &&
		response[1] == (index & 0xFF) && response[2] == ((index >> 8) & 0xFF) &&
		response[3] == subindex && memcmp (response + 4, suffix, sizeof (suffix)) == 0;
}

#endif
