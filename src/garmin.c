/*
 * Garmin Descent Mk1 USB storage downloading
 *
 * Copyright (C) 2018 Linus Torvalds
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

#include <stdio.h>
#include <dirent.h>

#include "garmin.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"

typedef struct garmin_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
} garmin_device_t;

static dc_status_t garmin_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t garmin_device_close (dc_device_t *abstract);

static const dc_device_vtable_t garmin_device_vtable = {
	sizeof(garmin_device_t),
	DC_FAMILY_GARMIN,
	NULL, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	garmin_device_foreach, /* foreach */
	NULL, /* timesync */
	garmin_device_close, /* close */
};

dc_status_t
garmin_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	garmin_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (garmin_device_t *) dc_device_allocate (context, &garmin_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
garmin_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	garmin_device_t *device = (garmin_device_t *) abstract;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
garmin_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	garmin_device_t *device = (garmin_device_t *) abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = 0;
	devinfo.serial = 0;
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = "Garmin";
	vendor.size = 6;
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	return status;
}
