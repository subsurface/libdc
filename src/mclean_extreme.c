/*
 * libdivecomputer
 *
 * Copyright (C) 2018 Jef Driesen
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

#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <stdint.h>

#include "mclean_extreme.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &mclean_extreme_device_vtable)

#define MAXRETRIES  14

#define STX         0x7E

#define CMD_COMPUTER		0xa0				// download computer configuration
#define CMD_SETCOMPUTER		0xa1				// upload computer configuration
#define CMD_DIVE			0xa3				// download specified dive configuration and samples
#define CMD_CLOSE			0xaa				// close connexion and turn off bluetooth

#define SZ_PACKET			512					// maximum packe payload length
#define SZ_SUMMARY			7					// size of the device fingerprint
#define SZ_CFG				0x002d				// size of the common dive/computer header
#define SZ_COMPUTER			0x0097				// size of the computer state dump
#define SZ_DIVE				0x005e				// size of the dive state dump
#define SZ_SAMPLE			0x0004				// size of the sample state dump

// private device parsing functions //////////////////////////////////////////////////////////////////////////////////////////

static uint16_t uint16(const unsigned char* buffer, int addr) { return (buffer[0 + addr] << 0) | (buffer[1 + addr] << 8); }
static uint32_t uint32(const unsigned char* buffer, int addr) { return (uint16(buffer, addr) << 0) | (uint16(buffer, addr + 2) << 16); }

static uint8_t device_format(const unsigned char* device) { return device[0x0000]; }
// static uint8_t device_gas_pO2(const unsigned char* device, int value) { return device[0x0001 + value * 2]; }
// static uint8_t device_gas_pHe(const unsigned char* device, int value) { return device[0x0001 + 1 + value * 2]; }
// static bool device_gas_enabled(const unsigned char* device, int value) { return (device[0x0011] & (1 << value)) != 0; }
// static uint8_t device_setpoint(const unsigned char* device, int value) { return device[0x0013 + value]; }
// static bool device_setpoint_enabled(const unsigned char* device, int value) { return (device[device, 0x0016] & (1 << value)) != 0; }
// static bool device_metric(unsigned char* device) { return device[0x0018] != 0; }
static uint16_t device_name(const unsigned char* device) { return uint16(device, 0x0019); }
// static uint16_t device_Vasc(const unsigned char* device) { return uint16(device, 0x001c); }
// static uint16_t device_Psurf(const unsigned char* device) { return uint16(device, 0x001e); }
// static uint8_t device_gfs_index(const unsigned char* device) { return device[0x0020]; }
// static uint8_t device_gflo(const unsigned char* device) { return device[0x0021]; }
// static uint8_t device_gfhi(const unsigned char* device) { return device[0x0022]; }
// static uint8_t device_density_index(const unsigned char* device) { return device[0x0023]; }
// static uint16_t device_ppN2_limit(const unsigned char* device) { return uint16(device, 0x0024); }
// static uint16_t device_ppO2_limit(const unsigned char* device) { return uint16(device, 0x0026); }
// static uint16_t device_ppO2_bottomlimit(const unsigned char* device) { return uint16(device, 0x0028); }
// static uint16_t device_density_limit(const unsigned char* device) { return uint16(device, 0x002a); }
// static uint8_t device_operatingmode(const unsigned char* device) { return device[0x002c]; }

// static uint16_t device_inactive_timeout(const unsigned char* device) { return uint16(device, SZ_CFG + 0x0008); }
// static uint16_t device_dive_timeout(const unsigned char* device) { return uint16(device, SZ_CFG + 0x000a); }
// static uint16_t device_log_period(const unsigned char* device) { return device[SZ_CFG + 0x000c]; }
// static uint16_t device_log_timeout(const unsigned char* device) { return uint16(device, SZ_CFG + 0x000e); }
// static uint8_t device_brightness_timeout(const unsigned char* device) { return device[SZ_CFG + 0x0010]; }
// static uint8_t device_brightness(const unsigned char* device) { return device[SZ_CFG + 0x0012]; }
// static uint8_t device_colorscheme(const unsigned char* device) { return device[SZ_CFG + 0x0013]; }
// static uint8_t device_language(const unsigned char* device) { return device[SZ_CFG + 0x0014]; }
// static uint8_t device_batterytype(const unsigned char* device) { return device[SZ_CFG + 0x0015]; }
// static uint16_t device_batterytime(const unsigned char* device) { return uint16(device, SZ_CFG + 0x0014); }
// static uint8_t device_button_sensitivity(const unsigned char* device) { return device[SZ_CFG + 0x0016]; }
// static uint8_t device_orientation(const unsigned char* device) { return device[SZ_CFG + 0x0049]; }
// static const char* device_owner(const unsigned char* device) { return (const char*)& device[SZ_CFG + 0x004a]; }

// private dive parsing functions //////////////////////////////////////////////////////////////////////////////////////////

static uint8_t dive_format(const unsigned char* dive) { return dive[0x0000]; }
static uint16_t dive_samples_cnt(const unsigned char* dive) { return uint16(dive, 0x005c); }

 ///////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct mclean_extreme_device_t {
	dc_device_t base;
	dc_iostream_t* iostream;
	unsigned char fingerprint[SZ_SUMMARY];
	uint8_t data[SZ_COMPUTER];
} mclean_extreme_device_t;

static dc_status_t mclean_extreme_device_set_fingerprint(dc_device_t* abstract, const unsigned char data[], unsigned int size);
static dc_status_t mclean_extreme_device_foreach(dc_device_t* abstract, dc_dive_callback_t callback, void* userdata);
static dc_status_t mclean_extreme_device_close(dc_device_t* abstract);

static const dc_device_vtable_t mclean_extreme_device_vtable = {
	sizeof(mclean_extreme_device_t),
	DC_FAMILY_MCLEAN_EXTREME,
	mclean_extreme_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	mclean_extreme_device_foreach, /* foreach */
	NULL, /* timesync */
	mclean_extreme_device_close, /* close */
};

static unsigned short
checksum_crc(const unsigned char data[], unsigned int size, unsigned short init)
{
	unsigned short crc = init;
	for (unsigned int i = 0; i < size; ++i) {
		crc ^= data[i] << 8;
		if (crc & 0x8000) {
			crc <<= 1;
			crc ^= 0x1021;
		}
		else {
			crc <<= 1;
		}
	}

	return crc;
}

static dc_status_t
mclean_extreme_send(mclean_extreme_device_t* device, unsigned char cmd, const unsigned char data[], size_t size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t* abstract = (dc_device_t*)device;
	unsigned short crc = 0;

	if (device_is_cancelled(abstract))
		return DC_STATUS_CANCELLED;

	if (size > SZ_PACKET)
		return DC_STATUS_INVALIDARGS;

	// Setup the data packet
	unsigned char packet[SZ_PACKET + 11] = {
		STX,
		0x00,
		(size >> 0) & 0xFF,
		(size >> 8) & 0xFF,
		(size >> 16) & 0xFF,
		(size >> 24) & 0xFF,
		cmd,
	};
	if (size) {
		memcpy(packet + 7, data, size);
	}
	crc = checksum_crc(packet + 1, size + 6, 0);
	packet[size + 7] = (crc >> 8) & 0xFF;
	packet[size + 8] = (crc) & 0xFF;
	packet[size + 9] = 0x00;
	packet[size + 10] = 0x00;

	// Give the dive computer some extra time.
	dc_iostream_sleep(device->iostream, 300);

	// Send the data packet.
	status = dc_iostream_write(device->iostream, packet, size + 11, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(abstract->context, "Failed to send the command.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mclean_extreme_receive(mclean_extreme_device_t* device, unsigned char rsp, unsigned char data[], size_t max_size, size_t* actual_size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t* abstract = (dc_device_t*)device;
	unsigned char header[7];
	unsigned int nretries = 0;

	// Read the packet start byte.
	// Unfortunately it takes a relative long time, about 6-8 seconds,
	// before the STX byte arrives. Hence the standard timeout of one
	// second is not sufficient, and we need to retry a few times on
	// timeout. The advantage over using a single read operation with a
	// large timeout is that we can give the user a chance to cancel the
	// operation.
	while (1) {
		status = dc_iostream_read(device->iostream, header + 0, 1, NULL);
		if (status != DC_STATUS_SUCCESS) {
			if (status != DC_STATUS_TIMEOUT) {
				ERROR(abstract->context, "Failed to receive the packet start byte.");
				return status;
			}

			// Abort if the maximum number of retries is reached.
			if (nretries++ >= MAXRETRIES)
				return status;

			// Cancel if requested by the user.
			if (device_is_cancelled(abstract))
				return DC_STATUS_CANCELLED;

			// Try again.
			continue;
		}

		if (header[0] == STX)
			break;

		// Reset the retry counter.
		nretries = 0;
	}

	// Read the packet header.
	status = dc_iostream_read(device->iostream, header + 1, sizeof(header) - 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(abstract->context, "Failed to receive the packet header.");
		return status;
	}

	// Verify the type byte.
	unsigned int type = header[1];
	if (type != 0x00) {
		ERROR(abstract->context, "Unexpected type byte (%02x).", type);
		return DC_STATUS_PROTOCOL;
	}

	// Verify the length.
	unsigned int length = array_uint32_le(header + 2);
	if (length > max_size) {
		ERROR(abstract->context, "Unexpected packet length (%u for %zu).", length, max_size);
		return DC_STATUS_PROTOCOL;
	}

	// Get the command type.
	unsigned int cmd = header[6];
	if (cmd != rsp) {
		ERROR(abstract->context, "Unexpected command byte (%02x).", cmd);
		return DC_STATUS_PROTOCOL;
	}

	size_t nbytes = 0;
	while (nbytes < length) {
		// Set the maximum packet size.
		size_t len = 1000;

		// Limit the packet size to the total size.
		if (nbytes + len > length)
			len = length - nbytes;

		// Read the packet payload.
		status = dc_iostream_read(device->iostream, data + nbytes, len, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR(abstract->context, "Failed to receive the packet payload.");
			return status;
		}

		nbytes += len;
	}

	// Read the packet checksum.
	unsigned char checksum[4];
	status = dc_iostream_read(device->iostream, checksum, sizeof(checksum), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(abstract->context, "Failed to receive the packet checksum.");
		return status;
	}

	// Verify the checksum.
	unsigned short crc = array_uint16_be(checksum);
	unsigned short ccrc = 0;
	ccrc = checksum_crc(header + 1, sizeof(header) - 1, ccrc);
	ccrc = checksum_crc(data, length, ccrc);
	if (crc != ccrc || checksum[2] != 0x00 || checksum[3] != 0) {
		ERROR(abstract->context, "Unexpected packet checksum.");
		return DC_STATUS_PROTOCOL;
	}

	if (actual_size != NULL) {
		// Return the actual length.
		*actual_size = length;
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
mclean_extreme_device_open(dc_device_t** out, dc_context_t* context, dc_iostream_t* iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	mclean_extreme_device_t* device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (mclean_extreme_device_t*)dc_device_allocate(context, &mclean_extreme_device_vtable);
	if (device == NULL) {
		ERROR(context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	memset(device->fingerprint, 0, sizeof(device->fingerprint));

	// Set the serial communication protocol (115200 8N1).
	status = dc_iostream_configure(device->iostream, 115200, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(context, "Failed to set the terminal attributes.");
		goto error_free;
	}

	// Set the timeout for receiving data (1000ms).
	status = dc_iostream_set_timeout(device->iostream, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(context, "Failed to set the timeout.");
		goto error_free;
	}

	// Send the init command.
	status = mclean_extreme_send(device, CMD_COMPUTER, NULL, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(context, "Failed to send the init command.");
		goto error_free;
	}

	// Read the device info.
	status = mclean_extreme_receive(device, CMD_COMPUTER, device->data, sizeof(device->data), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(context, "Failed to receive the device info.");
		goto error_free;
	}

	if (device_format(device->data) != 0) { /* bad device format */
		status = DC_STATUS_DATAFORMAT;
		ERROR(context, "Unsupported device format.");
		goto error_free;
	}

	*out = (dc_device_t*)device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate((dc_device_t*)device);
	return status;
}

static dc_status_t
mclean_extreme_device_close(dc_device_t* abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	mclean_extreme_device_t* device = (mclean_extreme_device_t*)abstract;

	status = mclean_extreme_send(device, CMD_CLOSE, NULL, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR(abstract->context, "Failed to send the exit command.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mclean_extreme_device_set_fingerprint(dc_device_t* abstract, const unsigned char data[], unsigned int size)
{
	mclean_extreme_device_t* device = (mclean_extreme_device_t*)abstract;

	if (size && size != sizeof(device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy(device->fingerprint, data, sizeof(device->fingerprint));
	else
		memset(device->fingerprint, 0, sizeof(device->fingerprint));

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mclean_extreme_device_readsamples(dc_device_t* abstract, uint8_t* dive)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	mclean_extreme_device_t* device = (mclean_extreme_device_t*)abstract;

	int samples_cnt = (dive[0x005c] << 0) + (dive[0x005d] << 8);						// number of samples to follow
	dive = &dive[SZ_DIVE];

	while (samples_cnt != 0) {
		unsigned char data[SZ_PACKET];					// buffer for read packet data
		size_t length;									// buffer for read packet length

		status = mclean_extreme_receive(device, CMD_DIVE, data, SZ_PACKET, &length);
		if (status != DC_STATUS_SUCCESS) {
			ERROR(abstract->context, "Failed to receive dive samples.");
			break;
		}

		int packet_cnt = length / SZ_SAMPLE;			// number of samples in the packet
		if (packet_cnt > samples_cnt) { /* too many samples received */
			status = DC_STATUS_DATAFORMAT;
			ERROR(abstract->context, "Too many dive samples received.");
			break;
		}
		if (length != packet_cnt * SZ_SAMPLE) { /* not an integer number of samples */
			status = DC_STATUS_DATAFORMAT;
			ERROR(abstract->context, "Partial samples received.");
			break;
		}

		memcpy(dive, data, packet_cnt * SZ_SAMPLE);		// append samples to dive buffer

		dive = &dive[packet_cnt * SZ_SAMPLE];			// move buffer write cursor
		samples_cnt -= packet_cnt;
	}

	return status;
}

static dc_status_t
mclean_extreme_device_foreach(dc_device_t* abstract, dc_dive_callback_t callback, void* userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	mclean_extreme_device_t* device = (mclean_extreme_device_t*)abstract;

	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

	int dives_cnt = device_name(device->data);

	progress.current = 0;
	progress.maximum = dives_cnt;
	device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

	for (int i = dives_cnt - 1; i >= 0; --i) {
		unsigned char data[512];			// buffer for read packet data
		size_t length;						// buffer for read packet length

		unsigned char id[] = { (unsigned char)i, (unsigned char)(i >> 8) };	// payload for CMD_DIVE request

		status = mclean_extreme_send(device, CMD_DIVE, id, sizeof(id));
		if (status != DC_STATUS_SUCCESS) {
			ERROR(abstract->context, "Failed to send the get dive command.");
			break;
		}

		status = mclean_extreme_receive(device, CMD_DIVE, data, 512, &length);
		if (status != DC_STATUS_SUCCESS) {
			ERROR(abstract->context, "Failed to receive dive header.");
			break;
		}

		if (dive_format(data) != 0) { /* can't understand the format */
			INFO(abstract->context, "Skipping unsupported dive format");
			break;
		}

		int cnt_samples = dive_samples_cnt(data);			// number of samples to follow
		size_t size = SZ_DIVE + cnt_samples * SZ_SAMPLE;	// total buffer size required for this dive
		uint8_t* dive = (uint8_t *)malloc(size);			// buffer for this dive

		if (dive == NULL) {
			status = DC_STATUS_NOMEMORY;
			break;
		}

		memcpy(dive, data, SZ_DIVE);									// copy data to dive buffer
		status = mclean_extreme_device_readsamples(abstract, dive);		// append samples to buffer

		if (status != DC_STATUS_SUCCESS) { /* failed to read dive samples */
			free(dive);	// cleanup
			break;		// stop downloading
		}

		if (callback && !callback(dive, size, dive, sizeof(device->fingerprint), userdata)) { /* cancelled by callback */
			free(dive);	// cleanup
			break;		// stop downloading
		}

		free(dive);
		progress.current = dives_cnt - 1 - i;
		device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);
	}

	return status;
}
