/*
 * libdivecomputer
 *
 * Copyright (C) 2008 Jef Driesen
 *           (C) 2017 Linus Torvalds
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

#include <stdlib.h> // malloc, free
#include <string.h>	// strncmp, strstr

#include "scubapro_g2.h"
#include "context-private.h"
#include "device-private.h"
#ifdef USBHID
#include "usbhid.h"
#endif
#include "array.h"
#include "platform.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &scubapro_g2_device_vtable)

#define RX_PACKET_SIZE 64
#define TX_PACKET_SIZE 32

#define ALADINSPORTMATRIX 0x17
#define ALADINSQUARE      0x22
#define G2                0x32

typedef struct scubapro_g2_device_t {
	dc_device_t base;
	unsigned int timestamp;
	unsigned int devtime;
	dc_ticks_t systime;
} scubapro_g2_device_t;

static dc_status_t scubapro_g2_device_set_fingerprint (dc_device_t *device, const unsigned char data[], unsigned int size);
static dc_status_t scubapro_g2_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t scubapro_g2_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t scubapro_g2_device_close (dc_device_t *abstract);

static const dc_device_vtable_t scubapro_g2_device_vtable = {
	sizeof(scubapro_g2_device_t),
	DC_FAMILY_UWATEC_G2,
	scubapro_g2_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	scubapro_g2_device_dump, /* dump */
	scubapro_g2_device_foreach, /* foreach */
	NULL, /* timesync */
	scubapro_g2_device_close /* close */
};

static dc_status_t
scubapro_g2_extract_dives (dc_device_t *device, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata);

static int receive_data(scubapro_g2_device_t *g2, unsigned char *buffer, int size, dc_event_progress_t *progress)
{
	dc_custom_io_t *io = _dc_context_custom_io(g2->base.context);
	while (size) {
		unsigned char buf[RX_PACKET_SIZE] = { 0 };
		size_t transferred = 0;
		dc_status_t rc;
		int len;

		rc = io->packet_read(io, buf, sizeof(buf), &transferred);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR(g2->base.context, "read interrupt transfer failed");
			return -1;
		}
		if (transferred < 1) {
			ERROR(g2->base.context, "incomplete read interrupt transfer (got empty packet)");
			return -1;
		}
		len = buf[0];
		if (transferred < len + 1) {
			ERROR(g2->base.context, "small packet read (got %zu, expected at least %d)", transferred, len + 1);
			return -1;
		}
		if (len >= sizeof(buf)) {
			ERROR(g2->base.context, "read interrupt transfer returns impossible packet size (%d)", len);
			return -1;
		}
		HEXDUMP (g2->base.context, DC_LOGLEVEL_DEBUG, "rcv", buf+1, len);
		if (len > size) {
			ERROR(g2->base.context, "receive result buffer too small - truncating");
			len = size;
		}
		memcpy(buffer, buf+1, len);
		size -= len;
		buffer += len;

		// Update and emit a progress event?
		if (progress) {
			progress->current += len;
			device_event_emit(&g2->base, DC_EVENT_PROGRESS, progress);
		}
	}
	return 0;
}

static dc_status_t
scubapro_g2_transfer(scubapro_g2_device_t *g2, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	dc_custom_io_t *io = _dc_context_custom_io(g2->base.context);
	unsigned char buf[TX_PACKET_SIZE+1] = { 0 }; // the +1 is for the report type byte
	dc_status_t status = DC_STATUS_SUCCESS;
	size_t transferred = 0;

	if (csize > sizeof(buf)-2) {
		ERROR(g2->base.context, "command too big (%d)", csize);
		return DC_STATUS_INVALIDARGS;
	}

	HEXDUMP (g2->base.context, DC_LOGLEVEL_DEBUG, "cmd", command, csize);

	buf[0] = 0;			// USBHID report type
	buf[1] = csize;			// command size
	memcpy(buf+2, command, csize);	// command bytes

	// BLE GATT protocol?
	if (io->packet_size < 64) {
		// No report type byte
		status = io->packet_write(io, buf+1, csize+1, &transferred);
	} else {
		status = io->packet_write(io, buf, sizeof(buf), &transferred);
	}

	if (status != DC_STATUS_SUCCESS) {
		ERROR(g2->base.context, "Failed to send the command.");
		return status;
	}

	if (receive_data(g2, answer, asize, NULL) < 0) {
		ERROR(g2->base.context, "Failed to receive the answer.");
		return DC_STATUS_IO;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
scubapro_g2_handshake (scubapro_g2_device_t *device, unsigned int model)
{
	dc_device_t *abstract = (dc_device_t *) device;

	// Command template.
	unsigned char answer[1] = {0};
	unsigned char command[5] = {0x00, 0x10, 0x27, 0, 0};

	// The vendor software does not do a handshake for the Aladin Sport Matrix,
	// so let's not do any either.
	if (model == ALADINSPORTMATRIX)
		return DC_STATUS_SUCCESS;

	// Handshake (stage 1).
	command[0] = 0x1B;
	dc_status_t rc = scubapro_g2_transfer (device, command, 1, answer, 1);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the answer.
	if (answer[0] != 0x01) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	// Handshake (stage 2).
	command[0] = 0x1C;
	rc = scubapro_g2_transfer (device, command, 5, answer, 1);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the answer.
	if (answer[0] != 0x01) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}

struct usb_id {
	unsigned int model;
	unsigned short vendor, device;
};
#define C_ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

static const struct usb_id *get_usb_id(unsigned int model)
{
	int i;
	static const struct usb_id model_to_usb[] = {
		{ G2,		0x2e6c, 0x3201 },	// Scubapro G2
		{ ALADINSQUARE,	0xc251, 0x2006 },	// Scubapro Aladin Square
	};

	for (i = 0; i < C_ARRAY_SIZE(model_to_usb); i++) {
		const struct usb_id *id = model_to_usb+i;

		if (id->model == model)
			return id;
	}
	return NULL;
};

dc_status_t
scubapro_g2_device_open(dc_device_t **out, dc_context_t *context, const char *name, unsigned int model)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	scubapro_g2_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (scubapro_g2_device_t *) dc_device_allocate (context, &scubapro_g2_device_vtable);
	if (device == NULL) {
		ERROR(context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}


	// Set the default values.
	device->timestamp = 0;
	device->systime = (dc_ticks_t) -1;
	device->devtime = 0;

	dc_custom_io_t *io = _dc_context_custom_io(context);
	if (io && io->packet_open)
		status = io->packet_open(io, context, name);
	else {
		const struct usb_id *id = get_usb_id(model);
		if (!id) {
			ERROR(context, "Unknown USB ID for Scubapro model %#04x", model);
			status = DC_STATUS_IO;
			goto error_free;
		}
#ifdef USBHID
		status = dc_usbhid_custom_io(context, id->vendor, id->device);
#else
		status = DC_STATUS_UNSUPPORTED;
#endif
	}

	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to open Scubapro G2 device");
		goto error_free;
	}

	// Perform the handshaking.
	status = scubapro_g2_handshake(device, model);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to handshake with the device.");
		goto error_close;
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_close:
	scubapro_g2_device_close((dc_device_t *) device);
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
scubapro_g2_device_close (dc_device_t *abstract)
{
	dc_custom_io_t *io = _dc_context_custom_io(abstract->context);

	return io->packet_close(io);
}


static dc_status_t
scubapro_g2_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	scubapro_g2_device_t *device = (scubapro_g2_device_t*) abstract;

	if (size && size != 4)
		return DC_STATUS_INVALIDARGS;

	if (size)
		device->timestamp = array_uint32_le (data);
	else
		device->timestamp = 0;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
scubapro_g2_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	scubapro_g2_device_t *device = (scubapro_g2_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	// Read the model number.
	unsigned char cmd_model[1] = {0x10};
	unsigned char model[1] = {0};
	rc = scubapro_g2_transfer (device, cmd_model, sizeof (cmd_model), model, sizeof (model));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Read the serial number.
	unsigned char cmd_serial[1] = {0x14};
	unsigned char serial[4] = {0};
	rc = scubapro_g2_transfer (device, cmd_serial, sizeof (cmd_serial), serial, sizeof (serial));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Read the device clock.
	unsigned char cmd_devtime[1] = {0x1A};
	unsigned char devtime[4] = {0};
	rc = scubapro_g2_transfer (device, cmd_devtime, sizeof (cmd_devtime), devtime, sizeof (devtime));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Store the clock calibration values.
	device->systime = dc_datetime_now ();
	device->devtime = array_uint32_le (devtime);

	// Update and emit a progress event.
	progress.current += 9;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	// Emit a clock event.
	dc_event_clock_t clock;
	clock.systime = device->systime;
	clock.devtime = device->devtime;
	device_event_emit (&device->base, DC_EVENT_CLOCK, &clock);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = model[0];
	devinfo.firmware = 0;
	devinfo.serial = array_uint32_le (serial);
	device_event_emit (&device->base, DC_EVENT_DEVINFO, &devinfo);

	// Command template.
	unsigned char command[9] = {0x00,
			(device->timestamp      ) & 0xFF,
			(device->timestamp >> 8 ) & 0xFF,
			(device->timestamp >> 16) & 0xFF,
			(device->timestamp >> 24) & 0xFF,
			0x10,
			0x27,
			0,
			0};

	// Data Length.
	command[0] = 0xC6;
	unsigned char answer[4] = {0};
	rc = scubapro_g2_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int length = array_uint32_le (answer);

	// Update and emit a progress event.
	progress.maximum = 4 + 9 + (length ? length + 4 : 0);
	progress.current += 4;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	if (length == 0)
		return DC_STATUS_SUCCESS;

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, length)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	unsigned char *data = dc_buffer_get_data (buffer);

	// Data.
	command[0] = 0xC4;
	rc = scubapro_g2_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int total = array_uint32_le (answer);

	// Update and emit a progress event.
	progress.current += 4;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	if (total != length + 4) {
		ERROR (abstract->context, "Received an unexpected size.");
		return DC_STATUS_PROTOCOL;
	}

	if (receive_data(device, data, length, &progress)) {
		ERROR (abstract->context, "Received an unexpected size.");
		return DC_STATUS_IO;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
scubapro_g2_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = scubapro_g2_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	rc = scubapro_g2_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


static dc_status_t
scubapro_g2_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	if (abstract && !ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	const unsigned char header[4] = {0xa5, 0xa5, 0x5a, 0x5a};

	// Search the data stream for start markers.
	unsigned int previous = size;
	unsigned int current = (size >= 4 ? size - 4 : 0);
	while (current > 0) {
		current--;
		if (memcmp (data + current, header, sizeof (header)) == 0) {
			// Get the length of the profile data.
			unsigned int len = array_uint32_le (data + current + 4);

			// Check for a buffer overflow.
			if (current + len > previous)
				return DC_STATUS_DATAFORMAT;

			if (callback && !callback (data + current, len, data + current + 8, 4, userdata))
				return DC_STATUS_SUCCESS;

			// Prepare for the next dive.
			previous = current;
			current = (current >= 4 ? current - 4 : 0);
		}
	}

	return DC_STATUS_SUCCESS;
}
