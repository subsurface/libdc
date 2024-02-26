/*
 * Dummy "stream" operations for USB storage
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "common-private.h"
#include "context-private.h"
#include "iostream-private.h"
#include "iterator-private.h"
#include "timer.h"

// Fake "device" that just contains the directory name that
// you can read out of the iostream. All the actual IO is
// up to you.
typedef struct dc_usbstorage_t {
	dc_iostream_t base;
	char pathname[PATH_MAX];
} dc_usbstorage_t;

static dc_status_t
dc_usb_storage_read (dc_iostream_t *iostream, void *data, size_t size, size_t *actual);

static const dc_iostream_vtable_t dc_usbstorage_vtable = {
	sizeof(dc_usbstorage_t),
	NULL, /* set_timeout */
	NULL, /* set_latency */
	NULL, /* set_break */
	NULL, /* set_dtr */
	NULL, /* set_rts */
	NULL, /* get_lines */
	NULL, /* get_available */
	NULL, /* configure */
	dc_usb_storage_read, /* read */
	NULL, /* write */
	NULL, /* flush */
	NULL, /* purge */
	NULL, /* sleep */
	NULL, /* close */
};

dc_status_t
dc_usb_storage_open (dc_iostream_t **out, dc_context_t *context, const char *name)
{
	dc_usbstorage_t *device = NULL;
	struct stat st;

	if (out == NULL || name == NULL)
		return DC_STATUS_INVALIDARGS;

	if (*name == '\0') {
		// that indicates an MTP device
		INFO (context, "Open MTP device");
	} else {
		INFO (context, "Open: name=%s", name);
		if (stat(name, &st) < 0 || !S_ISDIR(st.st_mode))
			return DC_STATUS_NODEVICE;
	}
	// Allocate memory.
	device = (dc_usbstorage_t *) dc_iostream_allocate (context, &dc_usbstorage_vtable, DC_TRANSPORT_USBSTORAGE);
	if (device == NULL) {
		SYSERROR (context, ENOMEM);
		return DC_STATUS_NOMEMORY;
	}

	strncpy(device->pathname, name, PATH_MAX);
	device->pathname[PATH_MAX-1] = 0;

	*out = (dc_iostream_t *) device;
	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_usb_storage_read (dc_iostream_t *iostream, void *data, size_t size, size_t *actual)
{
	dc_usbstorage_t *device = (dc_usbstorage_t *) iostream;
	size_t len = strlen(device->pathname);

	if (size <= len)
		return DC_STATUS_IO;
	memcpy(data, device->pathname, len+1);
	if (actual)
		*actual = len;
	return DC_STATUS_SUCCESS;
}
