/*
 * libdivecomputer
 *
 * Copyright (C) 2015 Claudiu Olteanu
 * base on code that is Copyright (C) 2008 Jef Driesen
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

#ifndef CUSTOM_SERIAL_H
#define CUSTOM_SERIAL_H

#include <string.h>
#include <stdlib.h>

#include "context.h"
#include "descriptor.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct serial_t serial_t;

typedef struct dc_serial_operations_t
{
	int (*open) (serial_t **device, dc_context_t *context, const char *name);
	int (*close) (serial_t *device);
	int (*read) (serial_t *device, void* data, unsigned int size);
	int (*write) (serial_t *device, const void* data, unsigned int size);
	int (*flush) (serial_t *device, int queue);
	int (*get_received) (serial_t *device);
	int (*get_transmitted) (serial_t *device);
	int (*set_timeout) (serial_t *device, long timeout);
} dc_serial_operations_t;

typedef struct dc_serial_t {
	serial_t *port;				//serial device port
	dc_transport_t type;			//the type of the transport (USB, SERIAL, IRDA, BLUETOOTH)
	void *data;				//specific data for serial device
	const dc_serial_operations_t *ops;	//reference to a custom set of operations
} dc_serial_t;

void dc_serial_init(dc_serial_t *device, void *data, const dc_serial_operations_t *ops);

dc_status_t dc_serial_native_open(dc_serial_t **serial, dc_context_t *context, const char *devname);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* CUSTOM_SERIAL_H */
