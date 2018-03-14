/*
 * libdivecomputer
 *
 * Copyright (C) 2017 Jef Driesen
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

#include <libdivecomputer/context.h>

#include "iostream-private.h"
#include "common-private.h"
#include "context-private.h"

/*
 * This is shamelessly stolen from src/custom.c, to make it
 * work with the subsurface custom_io model.
 */
typedef struct dc_custom_t {
	/* Base class. */
	dc_iostream_t base;
	/* Internal state. */
	dc_context_t *context;
} dc_custom_t;

static dc_status_t
dc_custom_set_timeout (dc_iostream_t *abstract, int timeout)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;
	dc_custom_io_t *io = _dc_context_custom_io(custom->context);

	if (!io->serial_set_timeout)
		return DC_STATUS_SUCCESS;

	return io->serial_set_timeout(io, timeout);
}

static dc_status_t
dc_custom_set_latency (dc_iostream_t *abstract, unsigned int value)
{
	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_custom_set_break (dc_iostream_t *abstract, unsigned int value)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;
	dc_custom_io_t *io = _dc_context_custom_io(custom->context);

	if (!io->serial_set_break)
		return DC_STATUS_SUCCESS;

	return io->serial_set_break(io, value);
}

static dc_status_t
dc_custom_set_dtr (dc_iostream_t *abstract, unsigned int value)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;
	dc_custom_io_t *io = _dc_context_custom_io(custom->context);

	if (!io->serial_set_dtr)
		return DC_STATUS_SUCCESS;

	return io->serial_set_dtr(io, value);
}

static dc_status_t
dc_custom_set_rts (dc_iostream_t *abstract, unsigned int value)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;
	dc_custom_io_t *io = _dc_context_custom_io(custom->context);

	if (!io->serial_set_rts)
		return DC_STATUS_SUCCESS;

	return io->serial_set_rts(io, value);
}

static dc_status_t
dc_custom_get_lines (dc_iostream_t *abstract, unsigned int *value)
{
	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_custom_get_available (dc_iostream_t *abstract, size_t *value)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;
	dc_custom_io_t *io = _dc_context_custom_io(custom->context);

	if (!io->serial_get_available)
		return DC_STATUS_SUCCESS;

	return io->serial_get_available(io, value);
}

static dc_status_t
dc_custom_configure (dc_iostream_t *abstract, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;
	dc_custom_io_t *io = _dc_context_custom_io(custom->context);

	if (!io->serial_configure)
		return DC_STATUS_SUCCESS;

	return io->serial_configure(io, baudrate, databits, parity, stopbits, flowcontrol);
}

static dc_status_t
dc_custom_read (dc_iostream_t *abstract, void *data, size_t size, size_t *actual)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;
	dc_custom_io_t *io = _dc_context_custom_io(custom->context);

	if (!io->serial_read)
		return DC_STATUS_SUCCESS;

	return io->serial_read(io, data, size, actual);
}

static dc_status_t
dc_custom_write (dc_iostream_t *abstract, const void *data, size_t size, size_t *actual)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;
	dc_custom_io_t *io = _dc_context_custom_io(custom->context);

	if (!io->serial_write)
		return DC_STATUS_SUCCESS;

	return io->serial_write(io, data, size, actual);
}

static dc_status_t
dc_custom_flush (dc_iostream_t *abstract)
{
	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_custom_purge (dc_iostream_t *abstract, dc_direction_t direction)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;
	dc_custom_io_t *io = _dc_context_custom_io(custom->context);

	if (!io->serial_purge)
		return DC_STATUS_SUCCESS;

	return io->serial_purge(io, direction);
}

static dc_status_t
dc_custom_sleep (dc_iostream_t *abstract, unsigned int milliseconds)
{
	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_custom_close (dc_iostream_t *abstract)
{
	dc_custom_t *custom = (dc_custom_t *) abstract;
	dc_custom_io_t *io = _dc_context_custom_io(custom->context);

	if (!io->serial_close)
		return DC_STATUS_SUCCESS;

	return io->serial_close(io);
}

static const dc_iostream_vtable_t dc_custom_vtable = {
	sizeof(dc_custom_t),
	dc_custom_set_timeout, /* set_timeout */
	dc_custom_set_latency, /* set_latency */
	dc_custom_set_break, /* set_break */
	dc_custom_set_dtr, /* set_dtr */
	dc_custom_set_rts, /* set_rts */
	dc_custom_get_lines, /* get_lines */
	dc_custom_get_available, /* get_received */
	dc_custom_configure, /* configure */
	dc_custom_read, /* read */
	dc_custom_write, /* write */
	dc_custom_flush, /* flush */
	dc_custom_purge, /* purge */
	dc_custom_sleep, /* sleep */
	dc_custom_close, /* close */
};

dc_status_t
dc_custom_io_serial_open(dc_iostream_t **out, dc_context_t *context, const char *name)
{
	dc_custom_io_t *io = _dc_context_custom_io(context);
	dc_custom_t *custom;

	custom = (dc_custom_t *) dc_iostream_allocate (context, &dc_custom_vtable);
	if (!custom) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	custom->context = context;
	*out = (dc_iostream_t *) custom;
	return io->serial_open(io, context, name);
}
