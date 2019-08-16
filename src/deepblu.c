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

#include "deepblu.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"

typedef struct deepblu_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[8];
} deepblu_device_t;

static dc_status_t deepblu_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t deepblu_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t deepblu_device_close (dc_device_t *abstract);

static const dc_device_vtable_t deepblu_device_vtable = {
	sizeof(deepblu_device_t),
	DC_FAMILY_DEEPBLU,
	deepblu_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	deepblu_device_foreach, /* foreach */
	NULL, /* timesync */
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
		ERROR(device->base.context, "Deepblu reply packet too big for buffer");
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

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	ERROR (device->base.context, "Deepblu Cosmiq+ set_fingerprint called");
	return DC_STATUS_SUCCESS;
}


static dc_status_t
deepblu_device_close (dc_device_t *abstract)
{
	deepblu_device_t *device = (deepblu_device_t *) abstract;

	ERROR (device->base.context, "Deepblu Cosmiq+ device close called");
	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepblu_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	deepblu_device_t *device = (deepblu_device_t *) abstract;

	ERROR (device->base.context, "Deepblu Cosmiq+ device_foreach called");
	return DC_STATUS_SUCCESS;
}
