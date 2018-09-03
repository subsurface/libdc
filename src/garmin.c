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
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "garmin.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"

typedef struct garmin_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[FIT_NAME_SIZE];
} garmin_device_t;

static dc_status_t garmin_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t garmin_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t garmin_device_close (dc_device_t *abstract);

static const dc_device_vtable_t garmin_device_vtable = {
	sizeof(garmin_device_t),
	DC_FAMILY_GARMIN,
	garmin_device_set_fingerprint, /* set_fingerprint */
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
	memset(device->fingerprint, 0, sizeof(device->fingerprint));

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
garmin_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	garmin_device_t *device = (garmin_device_t *)abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


static dc_status_t
garmin_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	garmin_device_t *device = (garmin_device_t *) abstract;

	return DC_STATUS_SUCCESS;
}

struct file_list {
	int nr, allocated;
	struct fit_name *array;
};

static int name_cmp(const void *a, const void *b)
{
	// Sort reverse string ordering (newest first), so use 'b,a'
	return strcmp(b,a);
}

/*
 * Get the FIT file list and sort it.
 *
 * Return number of files found.
*/
static int get_file_list(DIR *dir, struct file_list *files)
{
	struct dirent *de;

	while ((de = readdir(dir)) != NULL) {
		int len = strlen(de->d_name);

		if (len != FIT_NAME_SIZE-1)
			continue;
		if (strncasecmp(de->d_name + len - 4, ".FIT", 4))
			continue;

		if (files->nr == files->allocated) {
			struct fit_name *array;
			int n = 3*(files->allocated + 8)/2;
			size_t new_size;

			new_size = n * sizeof(array[0]);
			array = realloc(files->array, new_size);
			if (!array)
				return DC_STATUS_NOMEMORY;

			files->array = array;
			files->allocated = n;
		}

		memcpy(files->array + files->nr++, de->d_name, FIT_NAME_SIZE);
	}

	qsort(files->array, files->nr, sizeof(struct fit_name), name_cmp);
	return DC_STATUS_SUCCESS;
}

static dc_status_t
read_file(char *pathname, int pathlen, const char *name, dc_buffer_t *file)
{
	int fd, rc;

	pathname[pathlen] = '/';
	memcpy(pathname+pathlen+1, name, FIT_NAME_SIZE);
	fd = open(pathname, O_RDONLY);

	if (fd < 0)
		return DC_STATUS_IO;

	rc = DC_STATUS_SUCCESS;
	for (;;) {
		char buffer[4096];
		int n;

		n = read(fd, buffer, sizeof(buffer));
		if (!n)
			break;
		if (n > 0) {
			dc_buffer_append(file, buffer, n);
			continue;
		}
		rc = DC_STATUS_IO;
		break;
	}

	close(fd);
	return rc;
}

static dc_status_t
garmin_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	garmin_device_t *device = (garmin_device_t *) abstract;
	dc_parser_t *parser;
	char pathname[PATH_MAX];
	size_t pathlen;
	struct file_list files = { 0, 0, NULL };
	dc_buffer_t *file;
	DIR *dir;
	int rc;

	// Read the directory name from the iostream
	rc = dc_iostream_read(device->iostream, &pathname, sizeof(pathname), &pathlen);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// The actual dives are under the "Garmin/Activity/" directory
	// as FIT files, with names like "2018-08-20-10-23-30.fit".
	// Make sure our buffer is big enough.
	if (pathlen + strlen("/Garmin/Activity/") + FIT_NAME_SIZE + 2 > PATH_MAX)
		return DC_STATUS_IO;

	if (pathlen && pathname[pathlen-1] != '/')
		pathname[pathlen++] = '/';
	strcpy(pathname + pathlen, "Garmin/Activity");
	pathlen += strlen("Garmin/Activity");

	dir = opendir(pathname);
	if (!dir)
		return DC_STATUS_IO;

	// Get the list of FIT files
	rc = get_file_list(dir, &files);
	closedir(dir);
	if (rc != DC_STATUS_SUCCESS || !files.nr) {
		free(files.array);
		return rc;
	}

	// Can we find the fingerprint entry?
	for (int i = 0; i < files.nr; i++) {
		const char *name = files.array[i].name;

		if (memcmp(name, device->fingerprint, sizeof (device->fingerprint)))
			continue;

		// Found fingerprint, just cut the array short here
		files.nr = i;
		break;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = files.nr;
	progress.current = 0;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

#if 0
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
#endif

	file = dc_buffer_new (16384);
	if (file == NULL) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		free(files.array);
		return DC_STATUS_NOMEMORY;
	}
	if ((rc = garmin_parser_create(&parser, abstract->context) != DC_STATUS_SUCCESS)) {
		ERROR (abstract->context, "Failed to create parser for dive verification.");
		free(files.array);
		return rc;
	}

	for (int i = 0; i < files.nr; i++) {
		const char *name = files.array[i].name;
		const unsigned char *data;
		unsigned int size;

		if (device_is_cancelled(abstract)) {
			status = DC_STATUS_CANCELLED;
			break;
		}

		// Reset the membuffer, read the data
		dc_buffer_clear(file);
		dc_buffer_append(file, name, FIT_NAME_SIZE);

		status = read_file(pathname, pathlen, name, file);
		if (status != DC_STATUS_SUCCESS)
			break;

		data = dc_buffer_get_data(file);
		size = dc_buffer_get_size(file);

		if (!garmin_parser_is_dive(parser, data, size)) {
			DEBUG (abstract->context, "decided %s isn't a dive.", name);
			continue;
		}

		if (callback && !callback(data, size, name, FIT_NAME_SIZE, userdata))
			break;

		progress.current++;
		device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);
	}

	free(files.array);
	dc_parser_destroy(parser);
	return status;
}
