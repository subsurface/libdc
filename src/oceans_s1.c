// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2020 Linus Torvalds

#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

#include "oceans_s1.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &oceans_s1_device_vtable)

typedef struct oceans_s1_device_t {
	dc_device_t base;
	dc_iostream_t* iostream;
	unsigned char fingerprint[4];
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

	if (strncmp(buffer, result, strlen(result)))
		return DC_STATUS_IO;

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

	if (size && size != sizeof(s1->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy(s1->fingerprint, data, sizeof(s1->fingerprint));
	else
		memset(s1->fingerprint, 0, sizeof(s1->fingerprint));

	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceans_s1_device_foreach(dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceans_s1_device_t *s1 = (oceans_s1_device_t*)abstract;

	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

	progress.current = 0;
	progress.maximum = 100;
	device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

	// Fill in

	return status;
}
