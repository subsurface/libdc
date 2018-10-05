/*
 * libdivecomputer
 *
 * Copyright (C) 2016 Jef Driesen
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

#include <stddef.h>
#include <stdlib.h>
#include <assert.h>

#include "iostream-private.h"
#include "context-private.h"
#include "platform.h"

dc_iostream_t *
dc_iostream_allocate (dc_context_t *context, const dc_iostream_vtable_t *vtable, dc_transport_t transport)
{
	dc_iostream_t *iostream = NULL;

	assert(vtable != NULL);
	assert(vtable->size >= sizeof(dc_iostream_t));

	// Allocate memory.
	iostream = (dc_iostream_t *) malloc (vtable->size);
	if (iostream == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return iostream;
	}

	// Initialize the base class.
	iostream->vtable = vtable;
	iostream->context = context;
	iostream->transport = transport;

	return iostream;
}

void
dc_iostream_deallocate (dc_iostream_t *iostream)
{
	free (iostream);
}

int
dc_iostream_isinstance (dc_iostream_t *iostream, const dc_iostream_vtable_t *vtable)
{
	if (iostream == NULL)
		return 0;

	return iostream->vtable == vtable;
}

dc_transport_t
dc_iostream_get_transport (dc_iostream_t *iostream)
{
	if (iostream == NULL)
		return DC_TRANSPORT_NONE;

	return iostream->transport;
}

dc_status_t
dc_iostream_set_timeout (dc_iostream_t *iostream, int timeout)
{
	if (iostream == NULL || iostream->vtable->set_timeout == NULL)
		return DC_STATUS_SUCCESS;

	INFO (iostream->context, "Timeout: value=%i", timeout);

	return iostream->vtable->set_timeout (iostream, timeout);
}

dc_status_t
dc_iostream_set_latency (dc_iostream_t *iostream, unsigned int value)
{
	if (iostream == NULL || iostream->vtable->set_latency == NULL)
		return DC_STATUS_SUCCESS;

	INFO (iostream->context, "Latency: value=%i", value);

	return iostream->vtable->set_latency (iostream, value);
}

dc_status_t
dc_iostream_set_break (dc_iostream_t *iostream, unsigned int value)
{
	if (iostream == NULL || iostream->vtable->set_break == NULL)
		return DC_STATUS_SUCCESS;

	INFO (iostream->context, "Break: value=%i", value);

	return iostream->vtable->set_break (iostream, value);
}

dc_status_t
dc_iostream_set_dtr (dc_iostream_t *iostream, unsigned int value)
{
	if (iostream == NULL || iostream->vtable->set_dtr == NULL)
		return DC_STATUS_SUCCESS;

	INFO (iostream->context, "DTR: value=%i", value);

	return iostream->vtable->set_dtr (iostream, value);
}

dc_status_t
dc_iostream_set_rts (dc_iostream_t *iostream, unsigned int value)
{
	if (iostream == NULL || iostream->vtable->set_rts == NULL)
		return DC_STATUS_SUCCESS;

	INFO (iostream->context, "RTS: value=%i", value);

	return iostream->vtable->set_rts (iostream, value);
}

dc_status_t
dc_iostream_get_lines (dc_iostream_t *iostream, unsigned int *value)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	unsigned int lines = 0;

	if (iostream == NULL || iostream->vtable->get_lines == NULL) {
		goto out;
	}

	status = iostream->vtable->get_lines (iostream, &lines);

	INFO (iostream->context, "Lines: value=%u", lines);

out:
	if (value)
		*value = lines;

	return status;
}

dc_status_t
dc_iostream_get_available (dc_iostream_t *iostream, size_t *value)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	size_t available = 0;

	if (iostream == NULL || iostream->vtable->get_available == NULL) {
		goto out;
	}

	status = iostream->vtable->get_available (iostream, &available);

	INFO (iostream->context, "Available: value=" DC_PRINTF_SIZE, available);

out:
	if (value)
		*value = available;

	return status;
}

dc_status_t
dc_iostream_configure (dc_iostream_t *iostream, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol)
{
	if (iostream == NULL || iostream->vtable->configure == NULL)
		return DC_STATUS_SUCCESS;

	INFO (iostream->context, "Configure: baudrate=%i, databits=%i, parity=%i, stopbits=%i, flowcontrol=%i",
		baudrate, databits, parity, stopbits, flowcontrol);

	return iostream->vtable->configure (iostream, baudrate, databits, parity, stopbits, flowcontrol);
}

dc_status_t
dc_iostream_read (dc_iostream_t *iostream, void *data, size_t size, size_t *actual)
{
	if (actual)
		*actual = 0;

	if (iostream == NULL || iostream->vtable->read == NULL)
		return DC_STATUS_IO;

	while (size) {
		dc_status_t status;
		size_t nbytes = 0;

		status = iostream->vtable->read (iostream, data, size, &nbytes);
		HEXDUMP (iostream->context, DC_LOGLEVEL_INFO, "Read", (unsigned char *) data, nbytes);

		/*
		 * If the reader is able to handle partial results,
		 * return them as such. NOTE! No need to add up a
		 * total, we will go through this loop only once
		 * in this case.
		 */
		if (actual) {
			*actual = nbytes;
			return status;
		}

		if (status != DC_STATUS_SUCCESS)
			return status;

		/*
		 * Continue reading to fill up the whole buffer,
		 * since the reader is not able to handle a
		 * partial result.
		 */
		data = (void *)(nbytes + (char *)data);
		size -= nbytes;
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_iostream_write (dc_iostream_t *iostream, const void *data, size_t size, size_t *actual)
{
	if (actual)
		*actual = 0;

	if (iostream == NULL || iostream->vtable->write == NULL)
		return DC_STATUS_IO;

	while (size) {
		dc_status_t status;
		size_t nbytes = 0;

		status = iostream->vtable->write (iostream, data, size, &nbytes);
		HEXDUMP (iostream->context, DC_LOGLEVEL_INFO, "Write", (const unsigned char *) data, nbytes);

		if (actual) {
			*actual = nbytes;
			return status;
		}

		if (status != DC_STATUS_SUCCESS)
			return status;

		data = (void *)(nbytes + (char *)data);
		size -= nbytes;
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_iostream_flush (dc_iostream_t *iostream)
{
	if (iostream == NULL || iostream->vtable->flush == NULL)
		return DC_STATUS_SUCCESS;

	INFO (iostream->context, "Flush: none");

	return iostream->vtable->flush (iostream);
}

dc_status_t
dc_iostream_purge (dc_iostream_t *iostream, dc_direction_t direction)
{
	if (iostream == NULL || iostream->vtable->purge == NULL)
		return DC_STATUS_SUCCESS;

	INFO (iostream->context, "Purge: direction=%u", direction);

	return iostream->vtable->purge (iostream, direction);
}

dc_status_t
dc_iostream_sleep (dc_iostream_t *iostream, unsigned int milliseconds)
{
	if (iostream == NULL || iostream->vtable->sleep == NULL)
		return DC_STATUS_SUCCESS;

	INFO (iostream->context, "Sleep: value=%u", milliseconds);

	return iostream->vtable->sleep (iostream, milliseconds);
}

dc_status_t
dc_iostream_close (dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	if (iostream == NULL)
		return DC_STATUS_SUCCESS;

	if (iostream->vtable->close) {
		status = iostream->vtable->close (iostream);
	}

	dc_iostream_deallocate (iostream);

	return status;
}

const char *
dc_iostream_get_name (dc_iostream_t *iostream)
{
	if (iostream == NULL)
		return NULL;

	if (iostream->vtable->get_name)
		return iostream->vtable->get_name (iostream);

	return NULL;
}
