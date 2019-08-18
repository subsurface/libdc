/*
 * Deepblu Cosmiq+ downloading
 *
 * Copyright (C) 2019 Linus Torvalds
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <string.h>
#include <stdlib.h>

#include "deepblu.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"

// "Write state"?
#define CMD_SETTIME	0x20	// Send 6 byte date-time, get single-byte 00x00 ack
#define CMD_23		0x23	// Send 00/01 byte, get ack back? Some metric/imperial setting?

// "Read dives"?
#define CMD_GETDIVENR	0x40	// Send empty byte, get single-byte number of dives back
#define CMD_GETDIVE	0x41	// Send dive number (1-nr) byte, get dive stat length byte back
  #define RSP_DIVESTAT	0x42	//  .. followed by packets of dive stat for that dive of that length
#define CMD_GETPROFILE	0x43	// Send dive number (1-nr) byte, get dive profile length BE word back
  #define RSP_DIVEPROF  0x44	//  .. followed by packets of dive profile of that length

// "Read state"?
#define CMD_58		0x58	// Send empty byte, get single byte back ?? (0x52)
#define CMD_59		0x59	// Send empty byte, get six bytes back (00 00 07 00 00 00)
#define CMD_5b		0x5b	// Send empty byte, get six bytes back (00 21 00 14 00 01)
#define CMD_5c		0x5c	// Send empty byte, get six bytes back (13 88 00 46 20 00)
#define CMD_5d		0x5d	// Send empty byte, get six bytes back (19 00 23 0C 02 0E)
#define CMD_5f		0x5f	// Send empty byte, get six bytes back (00 00 07 00 00 00)

typedef struct deepblu_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[8];
} deepblu_device_t;

static dc_status_t deepblu_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t deepblu_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t deepblu_device_timesync(dc_device_t *abstract, const dc_datetime_t *datetime);
static dc_status_t deepblu_device_close (dc_device_t *abstract);

static const dc_device_vtable_t deepblu_device_vtable = {
	sizeof(deepblu_device_t),
	DC_FAMILY_DEEPBLU,
	deepblu_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	deepblu_device_foreach, /* foreach */
	deepblu_device_timesync, /* timesync */
	deepblu_device_close, /* close */
};

// Maximum data in a packet. It's actually much
// less than this, since BLE packets are small and
// with the 7 bytes of headers and final newline
// and the HEX encoding, the actual maximum is
// just something like 6 bytes.
//
// But in theory the data could be done over
// multiple packets. That doesn't seem to be
// the case in anything I've seen so far.
//
// Pick something small and easy to use for
// stack buffers.
#define MAX_DATA 20

static char *
write_hex_byte(unsigned char data, char *p)
{
	static const char hex[16] = "0123456789ABCDEF";
	*p++ = hex[data >> 4];
	*p++ = hex[data & 0xf];
	return p;
}

//
// Send a cmd packet.
//
// The format of the cmd on the "wire" is:
//  - byte '#'
//  - HEX char of cmd
//  - HEX char two's complement modular sum of packet data (including cmd/size)
//  - HEX char size of data as encoded in HEX
//  - n * HEX char data
//  - byte '\n'
// so you end up having 8 bytes of header/trailer overhead, and two bytes
// for every byte of data sent due to the HEX encoding.
//
static dc_status_t
deepblu_send_cmd(deepblu_device_t *device, const unsigned char cmd, const unsigned char data[], size_t size)
{
	char buffer[8+2*MAX_DATA], *p;
	unsigned char csum;
	int i;

	if (size > MAX_DATA)
		return DC_STATUS_INVALIDARGS;

	// Calculate packet csum
	csum = cmd + 2*size;
	for (i = 0; i < size; i++)
		csum += data[i];
	csum = -csum;

	// Fill the data buffer
	p = buffer;
	*p++ = '#';
	p = write_hex_byte(cmd, p);
	p = write_hex_byte(csum, p);
	p = write_hex_byte(size*2, p);
	for (i = 0; i < size; i++)
		p = write_hex_byte(data[i], p);
	*p++ = '\n';

	// .. and send it out
	return dc_iostream_write(device->iostream, buffer, p-buffer, NULL);
}

//
// Receive one 'line' of data
//
// The deepblu BLE protocol is ASCII line based and packetized.
// Normally one packet is one line, but it looks like the Nordic
// Semi BLE chip will sometimes send packets early (some internal
// serial buffer timeout?) with incompete data.
//
// So read packets until you get newline.
static dc_status_t
deepblu_recv_line(deepblu_device_t *device, unsigned char *buf, size_t size)
{
	while (1) {
		unsigned char buffer[20];
		size_t transferred = 0;
		dc_status_t status;

		status = dc_iostream_read(device->iostream, buffer, sizeof(buffer), &transferred);
		if (status != DC_STATUS_SUCCESS) {
			ERROR(device->base.context, "Failed to receive Deepblu reply packet.");
			return status;
		}
		if (transferred > size) {
			ERROR(device->base.context, "Deepblu reply packet with too much data (got %zu, expected %zu)", transferred, size);
			return DC_STATUS_IO;
		}
		if (!transferred) {
			ERROR(device->base.context, "Empty Deepblu reply packet");
			return DC_STATUS_IO;
		}
		memcpy(buf, buffer, transferred);
		buf += transferred;
		size -= transferred;
		if (buf[-1] == '\n')
			break;
	}
	buf[-1] = 0;
	return DC_STATUS_SUCCESS;
}

static int
hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int
read_hex_byte(char *p)
{
	// This is negative if either of the nibbles is invalid
	return (hex_nibble(p[0]) << 4) | hex_nibble(p[1]);
}


//
// Receive a reply packet
//
// The reply packet has the same format as the cmd packet we
// send, except the first byte is '$' instead of '#'.
static dc_status_t
deepblu_recv_data(deepblu_device_t *device, const unsigned char expected, unsigned char *buf, size_t size, size_t *received)
{
	int len, i;
	dc_status_t status;
	char buffer[8+2*MAX_DATA];
	int cmd, csum, ndata;

	status = deepblu_recv_line(device, buffer, sizeof(buffer));
	if (status != DC_STATUS_SUCCESS)
		return status;

	// deepblu_recv_line() always zero-terminates the result
	// if it returned success, and has removed the final newline.
	len = strlen(buffer);
	HEXDUMP(device->base.context, DC_LOGLEVEL_DEBUG, "rcv", buffer, len);

	// A valid reply should always be at least 7 characters: the
	// initial '$' and the three header HEX bytes.
	if (len < 8 || buffer[0] != '$') {
		ERROR(device->base.context, "Invalid Deepblu reply packet");
		return DC_STATUS_IO;
	}

	cmd = read_hex_byte(buffer+1);
	csum = read_hex_byte(buffer+3);
	ndata = read_hex_byte(buffer+5);
	if ((cmd | csum | ndata) < 0) {
		ERROR(device->base.context, "non-hex Deepblu reply packet header");
		return DC_STATUS_IO;
	}

	// Verify the data length: it's the size of the HEX data,
	// and should also match the line length we got (the 7
	// is for the header data we already decoded above).
	if ((ndata & 1) || ndata != len - 7) {
		ERROR(device->base.context, "Deepblu reply packet data length does not match (claimed %d, got %d)", ndata, len-7);
		return DC_STATUS_IO;
	}

	if (ndata >> 1 > size) {
		ERROR(device->base.context, "Deepblu reply packet too big for buffer (ndata=%d, size=%zu)", ndata, size);
		return DC_STATUS_IO;
	}

	csum += cmd + ndata;

	for (i = 7; i < len; i += 2) {
		int byte = read_hex_byte(buffer + i);
		if (byte < 0) {
			ERROR(device->base.context, "Deepblu reply packet data not valid hex");
			return DC_STATUS_IO;
		}
		*buf++ = byte;
		csum += byte;
	}

	if (csum & 255) {
		ERROR(device->base.context, "Deepblu reply packet csum not valid (%x)", csum);
		return DC_STATUS_IO;
	}

	*received = ndata >> 1;
	return DC_STATUS_SUCCESS;
}

// Common communication pattern: send a command, expect data back with the same
// command byte.
static dc_status_t
deepblu_send_recv(deepblu_device_t *device, const unsigned char cmd,
	const unsigned char *data, size_t data_size,
	unsigned char *result, size_t result_size)
{
	dc_status_t status;
	size_t got;

	status = deepblu_send_cmd(device, cmd, data, data_size);
	if (status != DC_STATUS_SUCCESS)
		return status;
	status = deepblu_recv_data(device, cmd, result, result_size, &got);
	if (status != DC_STATUS_SUCCESS)
		return status;
	if (got != result_size) {
		ERROR(device->base.context, "Deepblu result size didn't match expected (expected %zu, got %zu)",
			result_size, got);
		return DC_STATUS_IO;
	}
	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepblu_recv_bulk(deepblu_device_t *device, const unsigned char cmd, unsigned char *buf, size_t len)
{
	while (len) {
		dc_status_t status;
		size_t got;

		status = deepblu_recv_data(device, cmd, buf, len, &got);
		if (status != DC_STATUS_SUCCESS)
			return status;
		if (got > len) {
			ERROR(device->base.context, "Deepblu bulk receive overflow");
			return DC_STATUS_IO;
		}
		buf += got;
		len -= got;
	}
	return DC_STATUS_SUCCESS;
}

dc_status_t
deepblu_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	deepblu_device_t *device;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (deepblu_device_t *) dc_device_allocate (context, &deepblu_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	memset(device->fingerprint, 0, sizeof(device->fingerprint));

	*out = (dc_device_t *) device;

	ERROR (context, "Deepblu Cosmiq+ open called");
	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepblu_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	deepblu_device_t *device = (deepblu_device_t *)abstract;

	ERROR (device->base.context, "Deepblu Cosmiq+ set_fingerprint called");
	HEXDUMP(device->base.context, DC_LOGLEVEL_DEBUG, "set_fingerprint", data, size);

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}

static unsigned char bcd(int val)
{
	if (val >= 0 && val < 100) {
		int high = val / 10;
		int low = val % 10;
		return (high << 4) | low;
	}
	return 0;
}

static dc_status_t
deepblu_device_timesync(dc_device_t *abstract, const dc_datetime_t *datetime)
{
	deepblu_device_t *device = (deepblu_device_t *)abstract;
	unsigned char result[1], data[6];
	dc_status_t status;
	size_t len;

	data[0] = bcd(datetime->year - 2000);
	data[1] = bcd(datetime->month);
	data[2] = bcd(datetime->day);
	data[3] = bcd(datetime->hour);
	data[4] = bcd(datetime->minute);
	data[5] = bcd(datetime->second);

	// Maybe also check that we received one zero byte (ack?)
	return deepblu_send_recv(device, CMD_SETTIME,
			data, sizeof(data),
			result, sizeof(result));
}

static dc_status_t
deepblu_device_close (dc_device_t *abstract)
{
	deepblu_device_t *device = (deepblu_device_t *) abstract;

	return DC_STATUS_SUCCESS;
}

static const char zero[MAX_DATA];

static dc_status_t
deepblu_download_dive(deepblu_device_t *device, unsigned char nr, dc_dive_callback_t callback, void *userdata)
{
	unsigned char header_len;
	unsigned char profilebytes[2];
	unsigned int profile_len;
	dc_status_t status;
	char header[256];
	unsigned char *profile;

	status = deepblu_send_recv(device,  CMD_GETDIVE, &nr, 1, &header_len, 1);
	if (status != DC_STATUS_SUCCESS)
		return status;
	status = deepblu_recv_bulk(device, RSP_DIVESTAT, header, header_len);
	if (status != DC_STATUS_SUCCESS)
		return status;
	memset(header + header_len, 0, 256 - header_len);

	status = deepblu_send_recv(device,  CMD_GETPROFILE, &nr, 1, profilebytes, sizeof(profilebytes));
	if (status != DC_STATUS_SUCCESS)
		return status;
	profile_len = (profilebytes[0] << 8) | profilebytes[1];

	profile = malloc(256 + profile_len);
	if (!profile) {
		ERROR (device->base.context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// We make the dive data be 256 bytes of header, followed by the profile data
	memcpy(profile, header, 256);

	status = deepblu_recv_bulk(device, RSP_DIVEPROF, profile+256, profile_len);
	if (status != DC_STATUS_SUCCESS)
		return status;

	if (callback)
		callback(profile, profile_len+256, header, header_len, userdata);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepblu_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	deepblu_device_t *device = (deepblu_device_t *) abstract;
	unsigned char nrdives, val;
	dc_status_t status;
	int i;

	val = 0;
	status = deepblu_send_recv(device,  CMD_GETDIVENR, &val, 1, &nrdives, 1);
	if (status != DC_STATUS_SUCCESS)
		return status;

	if (!nrdives)
		return DC_STATUS_SUCCESS;

	progress.maximum = nrdives;
	progress.current = 0;
	device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

	for (i = 1; i <= nrdives; i++) {
		status = deepblu_download_dive(device, i, callback, userdata);
		if (status != DC_STATUS_SUCCESS)
			return status;
		progress.current = i;
		device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);
	}

	return DC_STATUS_SUCCESS;
}
