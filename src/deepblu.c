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
