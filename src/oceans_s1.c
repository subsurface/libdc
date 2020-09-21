// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2020 Linus Torvalds

#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>

#include "oceans_s1.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"

#define S1_FINGERPRINT 32

typedef struct oceans_s1_device_t {
	dc_device_t base;
	dc_iostream_t* iostream;
	unsigned char fingerprint[S1_FINGERPRINT];
} oceans_s1_device_t;

static dc_status_t oceans_s1_device_set_fingerprint(dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t oceans_s1_device_foreach(dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t oceans_s1_device_close(dc_device_t *abstract);
static dc_status_t oceans_s1_device_timesync(dc_device_t *abstract, const dc_datetime_t *datetime);

static const dc_device_vtable_t oceans_s1_device_vtable = {
	sizeof(oceans_s1_device_t),
	DC_FAMILY_OCEANS_S1,
	oceans_s1_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	oceans_s1_device_foreach, /* foreach */
	oceans_s1_device_timesync, /* timesync */
	oceans_s1_device_close, /* close */
};

static dc_status_t
oceans_s1_write(oceans_s1_device_t *s1, const char *msg)
{
	return dc_iostream_write(s1->iostream, msg, strlen(msg), NULL);
}

static dc_status_t
oceans_s1_read(oceans_s1_device_t *s1, char *buf, size_t bufsz)
{
	size_t nbytes;
	dc_status_t status;

	status = dc_iostream_read(s1->iostream, buf, bufsz, &nbytes);
	if (status != DC_STATUS_SUCCESS)
		return status;
	if (nbytes < bufsz)
		buf[nbytes] = 0;
	return status;
}

#define BUFSZ 64

// Note how we don't rely on the return value of 'vsnprintf(), or on
// NUL termination because it's not portable.
static dc_status_t oceans_s1_printf(oceans_s1_device_t *s1, const char *fmt, ...)
{
	va_list ap;
	char buffer[BUFSZ];

	va_start(ap, fmt);
	vsnprintf(buffer, BUFSZ, fmt, ap);
	va_end(ap);
	buffer[BUFSZ-1] = 0;

	return oceans_s1_write(s1, buffer);
}

static dc_status_t oceans_s1_expect(oceans_s1_device_t *s1, const char *result)
{
	char buffer[BUFSZ];
	dc_status_t status;

	status = oceans_s1_read(s1, buffer, BUFSZ);
	if (status != DC_STATUS_SUCCESS)
		return status;

	if (strncmp(buffer, result, strlen(result))) {
		ERROR(s1->base.context, "Expected '%s' got '%s'", result, buffer);
		return DC_STATUS_IO;
	}

	return DC_STATUS_SUCCESS;
}

/*
 * The "blob mode" is sends stuff in bigger chunks with some binary
 * header and trailer.
 *
 * It seems to be a sequence of packets with 517 bytes of payload:
 * three bytes of header, 512 bytes of ASCII data, and two bytes of
 * trailer (data checksum?).
 *
 * We're supposed to start the sequence with a 'C' packet, and reply
 * to each 517-byte packet sequence with a '\006' packet.
 *
 * When there is no more data, the S1 will send us a '\004' packet,
 * which we'll ack with a final '\006' packet.
 *
 * The header is '\001' followed by block number (starting at 1),
 * followed by (255-block) number. So we can get a sequence of
 *
 *  01 01 fe <512 bytes> xx xx
 *  01 02 fd <512 bytes> xx xx
 *  01 03 fc <512 bytes> xx xx
 *  01 04 fb <512 bytes> xx xx
 *  01 05 fa <512 bytes> xx xx
 *  01 06 f9 <512 bytes> xx xx
 *  01 07 f8 <512 bytes> xx xx
 *  04
 *
 * And we should reply with that '\006' packet for each of those
 * entries.
 *
 * NOTE! The above is not in single BLE packets, although the
 * sequence blocks always start at a packet boundary.
 */
#define BLOB_BUFSZ 256
static dc_status_t oceans_s1_get_sequence(oceans_s1_device_t *s1, unsigned char seq, dc_buffer_t *res)
{
	unsigned char buffer[BLOB_BUFSZ];
	dc_status_t status;
	size_t nbytes;

	status = dc_iostream_read(s1->iostream, buffer, BLOB_BUFSZ, &nbytes);
	if (status != DC_STATUS_SUCCESS)
		return status;
	if (!nbytes)
		return DC_STATUS_IO;

	if (buffer[0] == 4)
		return DC_STATUS_DONE;

	if (nbytes <= 3 || buffer[0] != 1)
		return DC_STATUS_IO;

	if (buffer[1] != seq || buffer[2]+seq != 255)
		return DC_STATUS_IO;

	nbytes -= 3;
	dc_buffer_append(res, buffer+3, nbytes);
	while (nbytes < 512) {
		size_t got;

		status = dc_iostream_read(s1->iostream, buffer, BLOB_BUFSZ, &got);
		if (status != DC_STATUS_SUCCESS)
			return status;

		if (!got)
			return DC_STATUS_IO;

		// We should check the checksum if it is that?
		if (got + nbytes > 512)
			got = 512-nbytes;
		dc_buffer_append(res, buffer, got);
		nbytes += got;
	}
	return DC_STATUS_SUCCESS;
}

static dc_status_t oceans_s1_get_blob(oceans_s1_device_t *s1, const unsigned char **result)
{
	dc_status_t status;
	dc_buffer_t *res;
	unsigned char *data;
	size_t size;
	unsigned char seq;

	res = dc_buffer_new(0);
	if (!res)
		return DC_STATUS_NOMEMORY;

	// Tell the Oceans S1 to into some kind of block mode..
	//
	// The Oceans Android app uses a "Write Command" rather than
	// a "Write Request" for this, but it seems to not matter

	status = dc_iostream_write(s1->iostream, "C", 1, NULL);
	if (status != DC_STATUS_SUCCESS)
		return status;

	seq = 1;
	for (;;) {
		status = oceans_s1_get_sequence(s1, seq, res);
		if (status == DC_STATUS_DONE)
			break;

		if (status != DC_STATUS_SUCCESS) {
			dc_buffer_free(res);
			return status;
		}

		// Ack the packet sequence, and go look for the next one
		status = dc_iostream_write(s1->iostream, "\006", 1, NULL);
		if (status != DC_STATUS_SUCCESS)
			return status;
		seq++;
	}



	// Tell the Oceans S1 to exit block mode (??)
	status = dc_iostream_write(s1->iostream, "\006", 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		dc_buffer_free(res);
		return status;
	}

	size = dc_buffer_get_size(res);

	// NUL-terminate before getting buffer
	dc_buffer_append(res, "", 1);
	data = dc_buffer_get_data(res);

	/* Remove trailing whitespace */
	while (size && isspace(data[size-1]))
		data[--size] = 0;

	*result = data;
	return DC_STATUS_SUCCESS;
}

/*
 * The Oceans S1 uses the normal UNIX epoch time format: seconds
 * since 1-1-1970. In UTC format (so converting local time to UTC).
 */
static dc_status_t oceans_s1_device_timesync(dc_device_t *abstract, const dc_datetime_t *datetime)
{
	oceans_s1_device_t *s1 = (oceans_s1_device_t *) abstract;
	dc_ticks_t timestamp;
	dc_status_t status;

	timestamp = dc_datetime_mktime(datetime);
	if (timestamp < 0)
		return DC_STATUS_INVALIDARGS;

	timestamp += datetime->timezone;

	status = oceans_s1_printf(s1, "utc %lld\n", (long long) timestamp);
	if (status != DC_STATUS_SUCCESS)
		return status;

	return oceans_s1_expect(s1, "utc>ok");
}


/*
 * Oceans S1 initial sequence (all ASCII text with newlines):
 *
 *	Cmd			Reply				Comments
 *
 *	"utc"			"utc>ok 1592912375"		// TZ=UTC date -d"@1592912375"
 *	"battery"		"battery>ok 59%"
 *	"version"		"version>ok 1.1 42a7e564"	// Odd hex contant. Device ID?
 *	"utc 1592912364"	"utc>ok"			TZ=UTC date -d"@1592912364"
 *	"units 0"		"units>ok"
 *	"name TGludXM="		"name>ok"			// WTF?
 *	"dllist"		"dllist>xmr"
 *
 * At this point, the sequence changed and is no longer single packets
 * with a full line with newline termination.
 *
 * We send a single 'C' character as a GATT "Write Command" - 0x53 (so
 * not "Write Request" - 0x12).
 *
 * The dive computer replies with GATT packets that contains:
 *
 *  - binary three bytes: "\x01\x01\xfe"
 *
 *  - followed by ASCII text blob (note the single space indentation):
 *
 *	divelog v1,10s/sample
 *	 dive 1,0,21,1591372057
 *	 continue 612,10
 *	 enddive 3131,496
 *	 dive 2,0,21,1591372925
 *	 enddive 1535,277
 *	 dive 3,0,32,1591463368
 *	 enddive 1711,4515
 *	 dive 4,0,32,1591961688
 *	 continue 300,45
 *	 continue 391,238
 *	 continue 420,126
 *	 continue 236,17
 *	 enddive 1087,2636
 *	endlog
 *
 * Followed by a lot of newlines to pad out the packets.
 *
 * NOTE! The newlines are probably because the way the Nordic Semi UART
 * buffering works: it will buffer the packets until they are full, or
 * until a newline.
 *
 * Then some odd data: write a single '\x06' character and get a single
 * character reply of '\x04' (!?). Repeat, get a '\x13' byte back.
 *
 * NOTE! Again these single-byte things are GATT "write command", not
 * GATT "write request" things. They may be commands to the UART, not
 * data. Some kind of flow control? Or UART buffer control?
 *
 * Then it seems to go back to line-mode with the usual Write Request:
 *
 *	"dlget 4 5"	 "dlget>xmr"
 *
 * which puts us in that "blob" mode again, and we send a singler 'C'
 * character again, and now get that same '\x01\x01\xfe' binary data
 * followed by ASCII text blob (note the space indentation again):
 *
 *	divelog v1,10s/sample
 *	 dive 4,0,32,1591961688
 *	  365,13,1
 *	  382,13,51456
 *	  367,13,16640
 *	  381,13,49408
 *	  375,13,24576
 *	  355,13,16384
 *	  346,13,16384
 *	  326,14,16384
 *	  355,14,16384
 *	  394,14,24576
 *	  397,14,16384
 *	  434,14,49152
 *	  479,14,49152
 *	  488,14,16384
 *	  556,14,57344
 *	  616,14,49152
 *	  655,14,49152
 *	  738,14,49152
 *	  800,14,57344
 *	  800,14,49408
 *	  834,14,16640
 *	  871,14,24832
 *	  860,14,16640
 *	  860,14,16640
 *	  815,14,24832
 *	  738,14,16640
 *	  707,14,16640
 *	  653,14,24832
 *	  647,13,16640
 *	  670,13,16640
 *	  653,13,24832
 *	  ...
 *	 continue 236,17
 *	  227,13,57600
 *	  238,14,16640
 *	  267,14,24832
 *	  283,14,16384
 *	  272,14,16384
 *	  303,14,24576
 *	  320,14,16384
 *	  318,14,16384
 *	  318,14,16384
 *	  335,14,24576
 *	  332,14,16384
 *	  386,14,16384
 *	  417,14,24576
 *	  244,14,16640
 *	  71,14,16640
 *	 enddive 1087,2636
 *	endlog
 *
 * Where the samples seem to be
 *  - 'depth in cm'
 *  - 'temperature in °C' (??)
 *  - 'hex value flags' (??)
 *
 * Repeat with 'dlget 3 4', 'dlget 2 3', 'dlget 1 2'.
 *
 * Done.
 */
dc_status_t
oceans_s1_device_open(dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	char buffer[128];
	dc_status_t status = DC_STATUS_SUCCESS;
	oceans_s1_device_t *s1 = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	s1 = (oceans_s1_device_t*)dc_device_allocate(context, &oceans_s1_device_vtable);
	if (s1 == NULL) {
		ERROR(context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	s1->iostream = iostream;
	memset(s1->fingerprint, 0, sizeof(s1->fingerprint));

	*out = (dc_device_t*)s1;

	// Do minimal verification that we can talk to it
	// as part of the open.
	status = oceans_s1_write(s1, "utc\n");
	if (status != DC_STATUS_SUCCESS)
		return status;

	status = oceans_s1_read(s1, buffer, sizeof(buffer));
	if (status != DC_STATUS_SUCCESS)
		return status;
	if (memcmp(buffer, "utc>ok", 6))
		return DC_STATUS_IO;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceans_s1_device_close(dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceans_s1_device_t *s1 = (oceans_s1_device_t*)abstract;

	// Fill in

	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceans_s1_device_set_fingerprint(dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	oceans_s1_device_t *s1 = (oceans_s1_device_t*)abstract;

	if (size > sizeof(s1->fingerprint))
		return DC_STATUS_INVALIDARGS;

	memset(s1->fingerprint, 0, sizeof(s1->fingerprint));
	memcpy(s1->fingerprint, data, size);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
get_dive_list(oceans_s1_device_t *s1, const unsigned char **list)
{
	dc_status_t status;

	status = oceans_s1_write(s1, "dllist\n");
	if (status != DC_STATUS_SUCCESS)
		return status;

	status = oceans_s1_expect(s1, "dllist>xmr");
	if (status != DC_STATUS_SUCCESS)
		return status;

	return oceans_s1_get_blob(s1, list);
}

static dc_status_t
get_one_dive(oceans_s1_device_t *s1, int nr, const unsigned char **dive)
{
	dc_status_t status;

	status = oceans_s1_printf(s1, "dlget %d %d\n", nr, nr+1);
	if (status != DC_STATUS_SUCCESS)
		return status;

	status = oceans_s1_expect(s1, "dlget>xmr");
	if (status != DC_STATUS_SUCCESS)
		return status;

	return oceans_s1_get_blob(s1, dive);
}

static const unsigned char *get_string_line(const unsigned char **blob)
{
	const unsigned char *in = *blob;
	const unsigned char *line;
	unsigned char c;

	*blob = NULL;
	if (!in)
		return NULL;

	while (isspace(*in))
		in++;

	if (!*in)
		return NULL;

	line = in;
	while ((c = *in) != 0) {
		if (c == '\r' || c == '\n')
			break;
		in++;
	}
	*blob = in;
	return line;
}

// The 'unknown' field is probably the dive mode
// 'date' is seconds since UNIX epoch (the usual "local time as GMT")
// depth and duration are in cm and seconds
// The fingerprint is just the 'dive' line padded with NUL characters
struct s1_dive {
	int nr, unknown, o2;
	long long date;
	unsigned maxdepth, duration;
	unsigned char fingerprint[S1_FINGERPRINT];
	struct s1_dive *next;
};

/*
 * React to the "dive x,y,z,date" line.
 *
 * Allocate the dive.
 */
static struct s1_dive *
s1_alloc_dive(const unsigned char *line, size_t len)
{
	struct s1_dive *dive;
	int nr, unknown, o2;
	long long date;

	if (sscanf(line, "dive %d,%d,%d,%lld", &nr, &unknown, &o2, &date) != 4)
		return NULL;

	dive = malloc(sizeof(*dive));
	if (dive) {
		memset(dive, 0, sizeof(*dive));

		dive->nr = nr;
		dive->unknown = unknown;
		dive->o2 = o2;
		dive->date = date;
		dive->next = NULL;

		if (len >= S1_FINGERPRINT)
			len = S1_FINGERPRINT-1;
		memcpy(dive->fingerprint, line, len);
	}
	return dive;
}

/*
 * React to the "enddive x,y" line
 *
 * Add a dive to the dive list, sorted with newest dive first
 *
 * I'm not sure if the dive list is always presented sorted by the
 * Oceans S1, but it arrives in the reverse order of what we want
 * (we want newest first, it lists them oldest first). So we need
 * to switch the order, and we might as well make sure it's sorted
 * while doing that.
 *
 * If it always comes sorted from the Oceans S1, the while () loop
 * here won't ever actually loop, so there's no real cost to this
 * (not that CPU time here matters).
 */
static int
s1_add_dive(struct s1_dive *dive, struct s1_dive **list, const unsigned char *line, size_t len)
{
	unsigned maxdepth, duration;
	struct s1_dive *next;

	if (!dive)
		return 0;

	if (sscanf(line, "enddive %u,%u", &maxdepth, &duration) != 2)
		return 0;

	dive->maxdepth = maxdepth;
	dive->duration = duration;
	while ((next = *list) != NULL) {
		if (dive->nr >= next->nr)
			break;
		list = &next->next;
	}
	dive->next = next;
	*list = dive;
	return 1;
}

static dc_status_t
oceans_s1_device_foreach(dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	int nr;
	struct s1_dive *divelist, *current_dive;
	const unsigned char *blob, *line;
	dc_status_t status = DC_STATUS_SUCCESS;
	oceans_s1_device_t *s1 = (oceans_s1_device_t*)abstract;

	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

	status = get_dive_list(s1, &blob);
	if (status != DC_STATUS_SUCCESS)
		return status;

	nr = 0;
	divelist = NULL;
	current_dive = NULL;
	while ((line = get_string_line(&blob)) != NULL) {
		int linelen = blob - line;
		const unsigned char *dive;

		/* We only care about 'dive' and 'enddive' lines */
		if (linelen < 8)
			continue;

		if (!memcmp(line, "dive ", 5)) {
			current_dive = s1_alloc_dive(line, linelen);
			continue;
		}

		if (memcmp(line, "enddive ", 8))
			continue;

		if (s1_add_dive(current_dive, &divelist, line, linelen))
			nr++;
		current_dive = NULL;
	}
	if (!nr)
		return DC_STATUS_SUCCESS;

	progress.current = 0;
	progress.maximum = nr;
	device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

	for (current_dive = divelist; current_dive; current_dive = current_dive->next) {
		const unsigned char *blob;

		if (!memcmp(current_dive->fingerprint, s1->fingerprint, S1_FINGERPRINT))
			break;

		if (device_is_cancelled(&s1->base))
			break;

		status = get_one_dive(s1, current_dive->nr, &blob);
		if (status != DC_STATUS_SUCCESS)
			return status;

		progress.current++;
		device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

		if (callback && !callback(blob, strlen(blob), current_dive->fingerprint, S1_FINGERPRINT, userdata))
			break;
	}

	return status;
}
