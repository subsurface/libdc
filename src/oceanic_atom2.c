/*
 * libdivecomputer
 *
 * Copyright (C) 2008 Jef Driesen
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

#include <string.h> // memcpy
#include <stdlib.h> // malloc, free

#include "oceanic_atom2.h"
#include "oceanic_common.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"
#include "ringbuffer.h"
#include "checksum.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &oceanic_atom2_device_vtable.base)

#define PROPLUSX   0x4552
#define VTX        0x4557
#define I750TC     0x455A
#define I770R      0x4651
#define GEO40      0x4653

#define MAXRETRIES 2
#define MAXDELAY   16
#define INVALID    0xFFFFFFFF

#define CMD_INIT      0xA8
#define CMD_VERSION   0x84
#define CMD_READ1     0xB1
#define CMD_READ8     0xB4
#define CMD_READ16    0xB8
#define CMD_READ16HI  0xF6
#define CMD_WRITE     0xB2
#define CMD_KEEPALIVE 0x91
#define CMD_QUIT      0x6A

#define ACK 0x5A
#define NAK 0xA5

typedef struct oceanic_atom2_device_t {
	oceanic_common_device_t base;
	dc_iostream_t *iostream;
	unsigned int sequence;
	unsigned int delay;
	unsigned int bigpage;
	unsigned char cache[256];
	unsigned int cached_page;
	unsigned int cached_highmem;
} oceanic_atom2_device_t;

static dc_status_t oceanic_atom2_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t oceanic_atom2_device_write (dc_device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size);
static dc_status_t oceanic_atom2_device_close (dc_device_t *abstract);

static const oceanic_common_device_vtable_t oceanic_atom2_device_vtable = {
	{
		sizeof(oceanic_atom2_device_t),
		DC_FAMILY_OCEANIC_ATOM2,
		oceanic_common_device_set_fingerprint, /* set_fingerprint */
		oceanic_atom2_device_read, /* read */
		oceanic_atom2_device_write, /* write */
		oceanic_common_device_dump, /* dump */
		oceanic_common_device_foreach, /* foreach */
		NULL, /* timesync */
		oceanic_atom2_device_close /* close */
	},
	oceanic_common_device_logbook,
	oceanic_common_device_profile,
};

static const oceanic_common_version_t aeris_f10_version[] = {
	{"FREEWAER \0\0 512K"},
	{"OCEANF10 \0\0 512K"},
	{"MUNDIAL R\0\0 512K"},
};

static const oceanic_common_version_t aeris_f11_version[] = {
	{"AERISF11 \0\0 1024"},
	{"OCEANF11 \0\0 1024"},
};

static const oceanic_common_version_t oceanic_atom1_version[] = {
	{"ATOM rev\0\0  256K"},
};

static const oceanic_common_version_t oceanic_atom2_version[] = {
	{"2M ATOM r\0\0 512K"},
};

static const oceanic_common_version_t oceanic_atom2a_version[] = {
	{"MANTA  R\0\0  512K"},
	{"INSIGHT2 \0\0 512K"},
	{"OCEVEO30 \0\0 512K"},
	{"ATMOSAI R\0\0 512K"},
	{"PROPLUS2 \0\0 512K"},
	{"OCEGEO20 \0\0 512K"},
	{"OCE GEO R\0\0 512K"},
	{"AQUAI200 \0\0 512K"},
	{"AQUA200C \0\0 512K"},
};

static const oceanic_common_version_t oceanic_atom2b_version[] = {
	{"ELEMENT2 \0\0 512K"},
	{"OCEVEO20 \0\0 512K"},
	{"TUSAZEN \0\0  512K"},
	{"AQUAI300 \0\0 512K"},
	{"HOLLDG03 \0\0 512K"},
	{"AQUAI100 \0\0 512K"},
	{"AQUA300C \0\0 \0\0\0\0"},
};

static const oceanic_common_version_t oceanic_atom2c_version[] = {
	{"2M EPIC r\0\0 512K"},
	{"EPIC1  R\0\0  512K"},
	{"AERIA300 \0\0 512K"},
};

static const oceanic_common_version_t oceanic_default_version[] = {
	{"OCE VT3 R\0\0 512K"},
	{"ELITET3 R\0\0 512K"},
	{"ELITET31 \0\0 512K"},
	{"DATAMASK \0\0 512K"},
	{"COMPMASK \0\0 512K"},
};

static const oceanic_common_version_t sherwood_wisdom_version[] = {
	{"WISDOM R\0\0  512K"},
};

static const oceanic_common_version_t oceanic_proplus3_version[] = {
	{"PROPLUS3 \0\0 512K"},
};

static const oceanic_common_version_t tusa_zenair_version[] = {
	{"TUZENAIR \0\0 512K"},
	{"AMPHOSSW \0\0 512K"},
	{"AMPHOAIR \0\0 512K"},
	{"VOYAGE2G \0\0 512K"},
	{"TUSTALIS \0\0 512K"},
};

static const oceanic_common_version_t oceanic_oc1_version[] = {
	{"OCWATCH R\0\0 1024"},
	{"OC1WATCH \0\0 1024"},
	{"OCSWATCH \0\0 1024"},
	{"AQUAI550 \0\0 1024"},
	{"AQUA550C \0\0 1024"},
};

static const oceanic_common_version_t oceanic_oci_version[] = {
	{"OCEANOCI \0\0 1024"},
};

static const oceanic_common_version_t oceanic_atom3_version[] = {
	{"OCEATOM3 \0\0 1024"},
	{"ATOM31  \0\0  1024"},
};

static const oceanic_common_version_t oceanic_vt4_version[] = {
	{"OCEANVT4 \0\0 1024"},
	{"OCEAVT41 \0\0 1024"},
	{"AERISAIR \0\0 1024"},
	{"SWVISION \0\0 1024"},
	{"XPSUBAIR \0\0 1024"},
};

static const oceanic_common_version_t hollis_tx1_version[] = {
	{"HOLLDG04 \0\0 2048"},
};

static const oceanic_common_version_t oceanic_veo1_version[] = {
	{"OCEVEO10 \0\0   8K"},
	{"AERIS XR1 NX R\0\0"},
};

static const oceanic_common_version_t oceanic_reactpro_version[] = {
	{"REACPRO2 \0\0 512K"},
};

// Like the i770R, there's some extended pattern for the last
// four digits. The serial communication apparently says "2048"
// for this, but the BLE version says "0001".
//
// The middle two digits are the FW version or something,
static const oceanic_common_version_t oceanic_proplusx_version[] = {
	{"OCEANOCX \0\0 \0\0\0\0"},
};

static const oceanic_common_version_t aeris_a300cs_version[] = {
	{"AER300CS \0\0 2048"},
	{"OCEANVTX \0\0 2048"},
	{"AQUAI750 \0\0 2048"},
};

// Not 100% sure what the pattern is.
// I've seen:
//
//   "AQUA770R 1A 0001"
//   "AQUA770R 1A 0090"
//
// from the same dive computer. On other ones, it's
// apparently the two middle digits that change, on
// the i770R it might be all of them.
static const oceanic_common_version_t aqualung_i770r_version[] = {
	{"AQUA770R \0\0 \0\0\0\0"},
};

static const oceanic_common_version_t aqualung_i450t_version[] = {
	{"AQUAI450 \0\0 2048"},
};

static const oceanic_common_layout_t aeris_f10_layout = {
	0x10000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0100, /* rb_logbook_begin */
	0x0D80, /* rb_logbook_end */
	32, /* rb_logbook_entry_size */
	0x0D80, /* rb_profile_begin */
	0x10000, /* rb_profile_end */
	0, /* pt_mode_global */
	2, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t aeris_f11_layout = {
	0x20000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0100, /* rb_logbook_begin */
	0x0D80, /* rb_logbook_end */
	32, /* rb_logbook_entry_size */
	0x0D80, /* rb_profile_begin */
	0x20000, /* rb_profile_end */
	0, /* pt_mode_global */
	3, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_default_layout = {
	0x10000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0A40, /* rb_profile_begin */
	0x10000, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_atom1_layout = {
	0x8000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0440, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0440, /* rb_profile_begin */
	0x8000, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_atom2a_layout = {
	0xFFF0, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0A40, /* rb_profile_begin */
	0xFE00, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_atom2b_layout = {
	0x10000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0A40, /* rb_profile_begin */
	0xFE00, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_atom2c_layout = {
	0xFFF0, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0A40, /* rb_profile_begin */
	0xFFF0, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t sherwood_wisdom_layout = {
	0xFFF0, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x03D0, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0A40, /* rb_profile_begin */
	0xFE00, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_proplus3_layout = {
	0x10000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x03E0, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0A40, /* rb_profile_begin */
	0xFE00, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t tusa_zenair_layout = {
	0xFFF0, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0A40, /* rb_profile_begin */
	0xFE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_oc1_layout = {
	0x20000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0240, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0A40, /* rb_profile_begin */
	0x1FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_oci_layout = {
	0x20000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x10C0, /* rb_logbook_begin */
	0x1400, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x1400, /* rb_profile_begin */
	0x1FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_atom3_layout = {
	0x20000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0400, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0A40, /* rb_profile_begin */
	0x1FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_vt4_layout = {
	0x20000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0420, /* rb_logbook_begin */
	0x0A40, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0A40, /* rb_profile_begin */
	0x1FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t hollis_tx1_layout = {
	0x40000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0780, /* rb_logbook_begin */
	0x1000, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x1000, /* rb_profile_begin */
	0x40000, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_veo1_layout = {
	0x0400, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0400, /* rb_logbook_begin */
	0x0400, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0400, /* rb_profile_begin */
	0x0400, /* rb_profile_end */
	0, /* pt_mode_global */
	0, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_reactpro_layout = {
	0xFFF0, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0400, /* rb_logbook_begin */
	0x0600, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0600, /* rb_profile_begin */
	0xFFF0, /* rb_profile_end */
	1, /* pt_mode_global */
	1, /* pt_mode_logbook */
	1, /* pt_mode_serial */
};

static const oceanic_common_layout_t oceanic_proplusx_layout = {
	0x440000, /* memsize */
	0x40000, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x1000, /* rb_logbook_begin */
	0x10000, /* rb_logbook_end */
	16, /* rb_logbook_entry_size */
	0x40000, /* rb_profile_begin */
	0x440000, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t aqualung_i770r_layout = {
	0x440000, /* memsize */
	0x40000, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x2000, /* rb_logbook_begin */
	0x10000, /* rb_logbook_end */
	16, /* rb_logbook_entry_size */
	0x40000, /* rb_profile_begin */
	0x440000, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t aeris_a300cs_layout = {
	0x40000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0900, /* rb_logbook_begin */
	0x1000, /* rb_logbook_end */
	16, /* rb_logbook_entry_size */
	0x1000, /* rb_profile_begin */
	0x3FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static const oceanic_common_layout_t aqualung_i450t_layout = {
	0x40000, /* memsize */
	0, /* highmem */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x10C0, /* rb_logbook_begin */
	0x1400, /* rb_logbook_end */
	16, /* rb_logbook_entry_size */
	0x1400, /* rb_profile_begin */
	0x3FE00, /* rb_profile_end */
	0, /* pt_mode_global */
	1, /* pt_mode_logbook */
	0, /* pt_mode_serial */
};

static dc_status_t
oceanic_atom2_packet (oceanic_atom2_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int crc_size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	if (device->delay) {
		dc_iostream_sleep (device->iostream, device->delay);
	}

	// Send the command to the dive computer.
	status = dc_iostream_write (device->iostream, command, csize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Get the correct ACK byte.
	unsigned int ack = ACK;
	if (command[0] == CMD_INIT || command[0] == CMD_QUIT) {
		ack = NAK;
	}

	// Receive the response (ACK/NAK) of the dive computer.
	unsigned char response = 0;
	status = dc_iostream_read (device->iostream, &response, 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the response of the dive computer.
	if (response != ack) {
		ERROR (abstract->context, "Unexpected answer start byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	if (asize) {
		// Receive the answer of the dive computer.
		status = dc_iostream_read (device->iostream, answer, asize, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return status;
		}

		// Verify the checksum of the answer.
		unsigned short crc, ccrc;
		if (crc_size == 2) {
			crc = array_uint16_le (answer + asize - 2);
			ccrc = checksum_add_uint16 (answer, asize - 2, 0x0000);
		} else {
			crc = answer[asize - 1];
			ccrc = checksum_add_uint8 (answer, asize - 1, 0x00);
		}
		if (crc != ccrc) {
			ERROR (abstract->context, "Unexpected answer checksum.");
			return DC_STATUS_PROTOCOL;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_atom2_serial_transfer (oceanic_atom2_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int crc_size)
{
	// Send the command to the device. If the device responds with an
	// ACK byte, the command was received successfully and the answer
	// (if any) follows after the ACK byte. If the device responds with
	// a NAK byte, we try to resend the command a number of times before
	// returning an error.

	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = oceanic_atom2_packet (device, command, csize, answer, asize, crc_size)) != DC_STATUS_SUCCESS) {
		if (rc != DC_STATUS_TIMEOUT && rc != DC_STATUS_PROTOCOL)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;

		// Increase the inter packet delay.
		if (device->delay < MAXDELAY)
			device->delay++;

		// Delay the next attempt.
		dc_iostream_sleep (device->iostream, 100);
		dc_iostream_purge (device->iostream, DC_DIRECTION_INPUT);
	}

	return DC_STATUS_SUCCESS;
}

/*
 * The BLE GATT packet size is up to 20 bytes and the format is:
 *
 * byte 0: <0xCD>
 *         Seems to always have this value. Don't ask what it means
 * byte 1: <d 1 c s s s s s>
 *          d=0 means "command", d=1 means "reply from dive computer"
 *          1 is always set, afaik
 *          c=0 means "last packet" in sequence, c=1 means "more packets coming"
 *          sssss is a 5-bit sequence number for packets
 * byte 2: <cmd seq>
 *          starts at 0 for the connection, incremented for each command
 * byte 3: <length of data>
 *          1-16 bytes of data per packet.
 * byte 4..n: <data>
 */
static dc_status_t
oceanic_atom2_ble_write(oceanic_atom2_device_t *device, const unsigned char command[], unsigned int csize)
{
	unsigned char buf[20];
	unsigned char cmd_seq = device->sequence;
	unsigned char pkt_seq;

	pkt_seq = 0;
	while (csize) {
		dc_status_t ret;
		unsigned char status = 0x40;
		unsigned int cpartial = csize;
		if (cpartial > 16) {
			cpartial = 16;
			status |= 0x20;
		}
		buf[0] = 0xcd;
		buf[1] = status | (pkt_seq & 31);
		buf[2] = cmd_seq;
		buf[3] = cpartial;
		memcpy(buf+4, command, cpartial);
		command += cpartial;
		csize -= cpartial;
		ret = dc_iostream_write(device->iostream, buf, 4+cpartial, NULL);
		if (ret != DC_STATUS_SUCCESS)
			return ret;
		pkt_seq++;
	}
	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceanic_atom2_ble_read(oceanic_atom2_device_t *device, unsigned char **result_p, unsigned int *size_p)
{
	unsigned char *result = NULL;
	unsigned int size = 0, allocated = 0;
	unsigned char buf[20];
	unsigned char cmd_seq = device->sequence;
	unsigned char pkt_seq;
	dc_status_t ret = DC_STATUS_SUCCESS;

	pkt_seq = 0;
	for (;;) {
		unsigned char status, expect;
		size_t transferred = 0;
		ret = dc_iostream_read(device->iostream, buf, sizeof(buf), &transferred);
		if (ret != DC_STATUS_SUCCESS)
			break;

		ret = DC_STATUS_IO;
		if (transferred < 5 || transferred > 20) {
			ERROR(device->base.base.context, "Odd BLE packet size %zd", transferred);
			break;
		}
		if (buf[0] != 0xcd)
			ERROR(device->base.base.context, "Odd first byte (got '%02x', expected 'cd'", buf[0]);

		// Verify status byte
		expect = 0xc0;
		expect |= (pkt_seq & 31);
		status = buf[1];
		if ((status & ~0x20) != expect)
			ERROR(device->base.base.context, "Odd status byte (got '%02x', expected '%02x'", buf[1], expect);

		// Verify command sequence byte
		expect = cmd_seq;
		if (buf[2] != expect)
			ERROR(device->base.base.context, "Odd cmd sequence byte (got '%02x', expected '%02x'", buf[2], expect);

		// Verify length byte
		expect = buf[3];
		if (expect < 1 || expect > 16) {
			ERROR(device->base.base.context, "Odd reply size byte (got %d, expected 1..16", buf[3]);
			break;
		}

		if (transferred < 4+expect) {
			ERROR(device->base.base.context, "Packet too small (got %zd bytes, expected at least %d bytes)", transferred, 4+expect);
			break;
		}

		if (size + expect > allocated) {
			unsigned int newsize = size + expect + 100;
			unsigned char *newalloc = realloc(result, newsize);
			if (!newalloc) {
				ret = DC_STATUS_NOMEMORY;
				break;
			}
			result = newalloc;
			allocated = newsize;
		}

		memcpy(result + size, buf+4, expect);
		size += expect;
		pkt_seq++;

		/* More packets? */
		if (status & 0x20)
			continue;

		ret = DC_STATUS_SUCCESS;
		break;
	}

	if (ret != DC_STATUS_SUCCESS) {
		free(result);
		size = 0;
		result = NULL;
	}
	*result_p = result;
	*size_p = size;
	return ret;
}

/*
 * Transfer a command and optionally read return data.
 *
 * NOTE! The NUL byte at the end of a command is a serial transfer thing,
 * and we remove it. The correct thing to do would be to add it on the
 * serial transfer side instead (or perhaps not send it at all, Jef says
 * it may be historical), but right now I've tried to minimize the changes
 * that the BLE transfer code made to the code, so instead this tries to
 * just skip the extraneous byte.
 */
static dc_status_t
oceanic_atom2_ble_transfer (oceanic_atom2_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int crc_size)
{
	unsigned char buf[20];
	unsigned char cmd_seq = device->sequence;
	unsigned char pkt_seq;
	dc_status_t ret = DC_STATUS_SUCCESS;
	int retry = 3;

	/*
	 * The serial commands have a NUL byte at the end. It's bogus.
	 * It should be added on the serial transfer side, not removed
	 * here.
	 */
	if (csize > 1 && csize < 8 && !command[csize-1])
		csize--;

retry:
	if (--retry < 0)
		return ret;

	ret = oceanic_atom2_ble_write(device, command, csize);
	if (ret != DC_STATUS_SUCCESS)
		return ret;

	pkt_seq = 0;
	if (answer) {
		unsigned char *buf;
		unsigned int size;
		ret = oceanic_atom2_ble_read(device, &buf, &size);
		if (ret != DC_STATUS_SUCCESS)
			goto retry;
		if (size > asize && buf[0] == ACK) {
			memcpy(answer, buf+1, asize);
			device->sequence++;
		} else {
			ERROR(device->base.base.context, "Result too small: got %d bytes, expected at least %d bytes", size, asize+1);
			ret = DC_STATUS_IO;
			goto retry;
		}
		free(buf);
	}

	return ret;
}

static dc_status_t
oceanic_atom2_transfer (oceanic_atom2_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int crc_size)
{
	if (dc_iostream_get_transport(device->iostream) == DC_TRANSPORT_BLE)
		return oceanic_atom2_ble_transfer(device, command, csize, answer, asize, crc_size);

	return oceanic_atom2_serial_transfer(device, command, csize, answer, asize, crc_size);
}

static dc_status_t
oceanic_atom2_quit (oceanic_atom2_device_t *device)
{
	// Send the command to the dive computer.
	unsigned char command[4] = {CMD_QUIT, 0x05, 0xA5, 0x00};
	dc_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), NULL, 0, 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


dc_status_t
oceanic_atom2_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream, unsigned int model)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceanic_atom2_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (oceanic_atom2_device_t *) dc_device_allocate (context, &oceanic_atom2_device_vtable.base);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	oceanic_common_device_init (&device->base);

	// Set the default values.
	device->iostream = iostream;
	device->delay = 0;
	device->sequence = 0;
	device->bigpage = 1; // no big pages
	device->cached_page = INVALID;
	device->cached_highmem = INVALID;
	memset(device->cache, 0, sizeof(device->cache));

	// Get the correct baudrate.
	unsigned int baudrate = 38400;
	if (model == VTX || model == I750TC || model == PROPLUSX || model == I770R || model == GEO40) {
		baudrate = 115200;
	}

	// Set the serial communication protocol (38400 8N1).
	status = dc_iostream_configure (device->iostream, baudrate, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free;
	}

	// Set the timeout for receiving data (1000 ms).
	status = dc_iostream_set_timeout (device->iostream, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	// Set the DTR line.
	status = dc_iostream_set_dtr (device->iostream, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the DTR line.");
		goto error_free;
	}

	// Clear the RTS line to reset the PIC inside the data cable as it
	// may not have have been previously cleared. This ensures that the
	// PIC will always start in a known state once RTS is set. Starting
	// in a known default state is very important as the PIC won't
	// respond to init commands unless it is in a default state.
	status = dc_iostream_set_rts (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to clear the RTS line.");
		goto error_free;
	}

	// Hold RTS clear for a bit to allow PIC to reset.
	dc_iostream_sleep (device->iostream, 100);

	// Set the RTS line.
	status = dc_iostream_set_rts (device->iostream, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the RTS line.");
		goto error_free;
	}

	// Give the interface 100 ms to settle and draw power up.
	dc_iostream_sleep (device->iostream, 100);

	// Make sure everything is in a sane state.
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	// Switch the device from surface mode into download mode. Before sending
	// this command, the device needs to be in PC mode (automatically activated
	// by connecting the device), or already in download mode.
	status = oceanic_atom2_device_version ((dc_device_t *) device, device->base.version, sizeof (device->base.version));
	if (status != DC_STATUS_SUCCESS) {
		goto error_free;
	}

	// Override the base class values.
	if (OCEANIC_COMMON_MATCH (device->base.version, aeris_f10_version)) {
		device->base.layout = &aeris_f10_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, aeris_f11_version)) {
		device->base.layout = &aeris_f11_layout;
		device->bigpage = 8;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_atom1_version)) {
		device->base.layout = &oceanic_atom1_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_atom2_version)) {
		if (array_uint16_be (device->base.version + 0x09) >= 0x3349) {
			device->base.layout = &oceanic_atom2a_layout;
		} else {
			device->base.layout = &oceanic_atom2c_layout;
		}
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_atom2a_version)) {
		device->base.layout = &oceanic_atom2a_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_atom2b_version)) {
		device->base.layout = &oceanic_atom2b_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_atom2c_version)) {
		device->base.layout = &oceanic_atom2c_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, sherwood_wisdom_version)) {
		device->base.layout = &sherwood_wisdom_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_proplus3_version)) {
		device->base.layout = &oceanic_proplus3_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, tusa_zenair_version)) {
		device->base.layout = &tusa_zenair_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_oc1_version)) {
		device->base.layout = &oceanic_oc1_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_oci_version)) {
		device->base.layout = &oceanic_oci_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_atom3_version)) {
		device->base.layout = &oceanic_atom3_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_vt4_version)) {
		device->base.layout = &oceanic_vt4_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, hollis_tx1_version)) {
		device->base.layout = &hollis_tx1_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_veo1_version)) {
		device->base.layout = &oceanic_veo1_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_reactpro_version)) {
		device->base.layout = &oceanic_reactpro_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_proplusx_version)) {
		device->base.layout = &oceanic_proplusx_layout;
		device->bigpage = 16;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, aqualung_i770r_version)) {
		device->base.layout = &aqualung_i770r_layout;
		device->bigpage = 16;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, aeris_a300cs_version)) {
		device->base.layout = &aeris_a300cs_layout;
		device->bigpage = 16;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, aqualung_i450t_version)) {
		device->base.layout = &aqualung_i450t_layout;
	} else if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_default_version)) {
		device->base.layout = &oceanic_default_layout;
	} else {
		WARNING (context, "Unsupported device detected (%s)!", device->base.version);
		device->base.layout = &oceanic_default_layout;
		if (memcmp(device->base.version + 12, "256K", 4) == 0) {
			device->base.layout = &oceanic_atom1_layout;
		} else if (memcmp(device->base.version + 12, "512K", 4) == 0) {
			device->base.layout = &oceanic_default_layout;
		} else if (memcmp(device->base.version + 12, "1024", 4) == 0) {
			device->base.layout = &oceanic_oc1_layout;
		} else if (memcmp(device->base.version + 12, "2048", 4) == 0) {
			device->base.layout = &hollis_tx1_layout;
		} else {
			device->base.layout = &oceanic_default_layout;
		}
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
oceanic_atom2_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Send the quit command.
	rc = oceanic_atom2_quit (device);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	return status;
}


dc_status_t
oceanic_atom2_device_keepalive (dc_device_t *abstract)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// Send the command to the dive computer.
	unsigned char command[4] = {CMD_KEEPALIVE, 0x05, 0xA5, 0x00};
	dc_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), NULL, 0, 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	/* No answer: increment sequence number manually */
	device->sequence++;
	return DC_STATUS_SUCCESS;
}

/*
 * The BLE communication sends a handshake packet that seems
 * to be a passphrase based on the BLE name of the device
 * (more specifically the serial number encoded in the name).
 *
 * The packet format is:
 *    0xe5
 *    < 8 bytes of passphrase >
 *    one-byte checksum of the passphrase.
 */
static dc_status_t
oceanic_atom2_send_ble_handshake(oceanic_atom2_device_t *device)
{
	unsigned char handshake[10] = { 0xe5, }, ack[1];
	const char *bt_name = dc_iostream_get_name(device->iostream);

	/*
	 * Allow skipping the handshake if no name. But the download will
	 * likely fail.
	 *
	 * The format of the name is something like 'FQ001124', where the
	 * two first letters indicate the kind of device it is, and the
	 * six digits are the serial number.
	 *
	 * Jef theorizes that 'FQ' in hexadecimal is 0x4651, which is
	 * the model number of the i770R.
	 */
	if (!bt_name || strlen(bt_name) < 8)
		return DC_STATUS_SUCCESS;

	/* Turn ASCII numbers into just raw byte values */
	for (int i = 0; i < 6; i++)
		handshake[i+1] = bt_name[i+2] - '0';

	/* Add simple checksum */
	handshake[9] = checksum_add_uint8(handshake+1, 8, 0x00);

	/*
	 * .. and send it off.
	 *
	 * NOTE! We don't expect any data back, but we do want the ACK.
	 */
	return oceanic_atom2_ble_transfer(device, handshake, sizeof(handshake), ack, 0, 0);
}

dc_status_t
oceanic_atom2_device_version (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < PAGESIZE)
		return DC_STATUS_INVALIDARGS;

	unsigned char answer[PAGESIZE + 1] = {0};
	unsigned char command[2] = {CMD_VERSION, 0x00};
	dc_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), answer, sizeof (answer), 1);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	memcpy (data, answer, PAGESIZE);

	if (dc_iostream_get_transport(device->iostream) == DC_TRANSPORT_BLE)
		rc = oceanic_atom2_send_ble_handshake(device);

	return rc;
}


static dc_status_t
oceanic_atom2_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;
	const oceanic_common_layout_t *layout = device->base.layout;

	if ((address % PAGESIZE != 0) ||
		(size    % PAGESIZE != 0))
		return DC_STATUS_INVALIDARGS;

	// Pick the correct read command and number of checksum bytes.
	unsigned char read_cmd = 0x00;
	unsigned int crc_size = 0;
	switch (device->bigpage) {
	case 1:
		read_cmd = CMD_READ1;
		crc_size = 1;
		break;
	case 8:
		read_cmd = CMD_READ8;
		crc_size = 1;
		break;
	case 16:
		read_cmd = CMD_READ16;
		crc_size = 2;
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	// Pick the best pagesize to use.
	unsigned int pagesize = device->bigpage * PAGESIZE;

	// High memory state.
	unsigned int highmem = 0;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Switch to the correct read command when entering the high memory area.
		if (layout->highmem && address >= layout->highmem && !highmem) {
			highmem = layout->highmem;
			read_cmd = CMD_READ16HI;
			crc_size = 2;
			pagesize = 16 * PAGESIZE;
		}

		// Calculate the page number after mapping the virtual high memory
		// addresses back to their physical address.
		unsigned int page = (address - highmem) / pagesize;

		if (page != device->cached_page || highmem != device->cached_highmem) {
			// Read the package.
			unsigned int number = highmem ? page : page * device->bigpage; // This is always PAGESIZE, even in big page mode.
			unsigned char answer[256 + 2] = {0};          // Maximum we support for the known commands.
			unsigned char command[4] = {read_cmd,
					(number >> 8) & 0xFF, // high
					(number     ) & 0xFF, // low
					0};
			dc_status_t rc = oceanic_atom2_transfer (device, command, sizeof (command), answer,  pagesize + crc_size, crc_size);
			if (rc != DC_STATUS_SUCCESS)
				return rc;

			// Cache the page.
			memcpy (device->cache, answer, pagesize);
			device->cached_page = page;
			device->cached_highmem = highmem;
		}

		unsigned int offset = address % pagesize;
		unsigned int length = pagesize - offset;
		if (nbytes + length > size)
			length = size - nbytes;

		memcpy (data, device->cache + offset, length);

		nbytes += length;
		address += length;
		data += length;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_atom2_device_write (dc_device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size)
{
	oceanic_atom2_device_t *device = (oceanic_atom2_device_t*) abstract;

	if ((address % PAGESIZE != 0) ||
		(size    % PAGESIZE != 0))
		return DC_STATUS_INVALIDARGS;

	// Invalidate the cache.
	device->cached_page = INVALID;
	device->cached_highmem = INVALID;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Prepare to write the package.
		unsigned int number = address / PAGESIZE;
		unsigned char prepare[4] = {CMD_WRITE,
				(number >> 8) & 0xFF, // high
				(number     ) & 0xFF, // low
				0x00};
		dc_status_t rc = oceanic_atom2_transfer (device, prepare, sizeof (prepare), NULL, 0, 0);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		// Write the package.
		unsigned char command[PAGESIZE + 2] = {0};
		memcpy (command, data, PAGESIZE);
		command[PAGESIZE] = checksum_add_uint8 (command, PAGESIZE, 0x00);
		rc = oceanic_atom2_transfer (device, command, sizeof (command), NULL, 0, 0);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		/* No answer, increment sequence number manually */
		device->sequence++;

		nbytes += PAGESIZE;
		address += PAGESIZE;
		data += PAGESIZE;
	}

	return DC_STATUS_SUCCESS;
}
