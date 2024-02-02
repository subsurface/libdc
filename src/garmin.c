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

#ifdef HAVE_LIBMTP
#include "libmtp.h"

#define GARMIN_VENDOR      0x091E
#define DESCENT_MK2        0x4CBA
#define DESCENT_MK2_APAC   0x4E76

// deal with ancient libmpt found on older Linux distros
#ifndef LIBMTP_FILES_AND_FOLDERS_ROOT
#define LIBMTP_FILES_AND_FOLDERS_ROOT 0xffffffff
#endif
#endif

typedef struct garmin_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[FIT_NAME_SIZE];
	unsigned int model;
#ifdef HAVE_LIBMTP
	unsigned char use_mtp;
	LIBMTP_mtpdevice_t *mtp_device;
#endif
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
garmin_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream, unsigned int model)
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
	device->model = model;

#ifdef HAVE_LIBMTP
	// for a Descent Mk2/Mk2i, we have to use MTP to access its storage;
	// for Garmin devices, the model number corresponds to the lower three nibbles of the USB product ID
	// in order to have only one entry for the Mk2, we don't use the Mk2/APAC model number in our code
	device->use_mtp = (model == (0x0FFF & DESCENT_MK2));
	device->mtp_device = NULL;
	DEBUG(context, "Found Garmin with model 0x%x which is a %s\n", model, (device->use_mtp ? "Mk2/Mk2i" : "Mk1"));
#endif

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
#ifdef HAVE_LIBMTP
	if (device->use_mtp && device->mtp_device)
		LIBMTP_Release_Device(device->mtp_device);
#endif
	return DC_STATUS_SUCCESS;
}

/*
 * NOTE! The fingerprint is only the 24 first bytes of this,
 * aka FIT_NAME_SIZE.
 */
#define FILE_NAME_SIZE 64

struct fit_file {
	char name[FILE_NAME_SIZE + 1];
	unsigned int mtp_id;
};

struct file_list {
	int nr, allocated;
	struct fit_file *array;
};

static int
char_to_int(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'Z')
		return c - 'A' + 10;
	return 0;
}

/* C4ND0302.fit -> 2022-04-23-13-03-02.fit */
static void
parse_short_name(const char *name, char *output)
{
	sprintf(output, "%d-%02d-%02d-%02d-%02d-%02d.fit", char_to_int(name[0]) + 2010, // Year
		char_to_int(name[1]), // Month
		char_to_int(name[2]), // Day
		char_to_int(name[3]), // Hour
		char_to_int(name[4]) * 10 + char_to_int(name[5]), // Minute
		char_to_int(name[6]) * 10 + char_to_int(name[7])); // Second
}

static int name_cmp(const void *_a, const void *_b)
{
	const struct fit_file *a = _a;
	const struct fit_file *b = _b;

	const char *a_name = a->name;
	const char *b_name = b->name;

	char a_buffer[FILE_NAME_SIZE];
	char b_buffer[FILE_NAME_SIZE];

	if (strlen(a_name) == 12) {
		parse_short_name(a_name, a_buffer);
		a_name = a_buffer;
	}

	if (strlen(b_name) == 12) {
		parse_short_name(b_name, b_buffer);
		b_name = b_buffer;
	}

	// Sort reverse string ordering (newest first), so use 'b,a'
	return strcmp(b_name, a_name);
}

/*
 * Get the FIT file list and sort it.
 *
 * Return number of files found.
*/

static int
check_filename(dc_device_t *abstract, const char *name)
{
	int len = strlen(name);

	if (len < 5)
		return 0;
	if (len >= FILE_NAME_SIZE)
		return 0;
	if (strncasecmp(name + len - 4, ".FIT", 4))
		return 0;

	DEBUG(abstract->context, "  %s - adding to list", name);
	return 1;
}

static dc_status_t
make_space(struct file_list *files)
{
	if (files->nr == files->allocated) {
		struct fit_file *array;
		int n = 3*(files->allocated + 8)/2;
		size_t new_size;

		new_size = n * sizeof(array[0]);
		array = realloc(files->array, new_size);
		if (!array)
			return DC_STATUS_NOMEMORY;
		files->array = array;
		files->allocated = n;
	}
	return DC_STATUS_SUCCESS;
}

static void
add_name(struct file_list *files, const char *name, unsigned int mtp_id)
{
	/*
	 * NOTE! This depends on the zero-padding that strncpy does.
	 *
	 * strncpy() doesn't just limit the size of the copy, it
	 * will zero-pad the end of the result buffer.
	 */
	struct fit_file *entry = files->array + files->nr++;
	strncpy(entry->name, name, FILE_NAME_SIZE);
	entry->name[FILE_NAME_SIZE] = 0; // ensure it's null-terminated
	entry->mtp_id = mtp_id;
}

static dc_status_t
get_file_list(dc_device_t *abstract, DIR *dir, struct file_list *files)
{
	struct dirent *de;

	DEBUG(abstract->context, "Iterating over Garmin files");
	while ((de = readdir(dir)) != NULL) {
		if (!check_filename(abstract, de->d_name))
			continue;

		dc_status_t rc = make_space(files);
		if (rc != DC_STATUS_SUCCESS)
			return rc;
		add_name(files, de->d_name, 0);
	}
	DEBUG(abstract->context, "Found %d files", files->nr);

	if (files->array)
		qsort(files->array, files->nr, sizeof(struct fit_file), name_cmp);
	return DC_STATUS_SUCCESS;
}

#ifdef HAVE_LIBMTP
static unsigned int
mtp_get_folder_id(dc_device_t *abstract, LIBMTP_mtpdevice_t *device, LIBMTP_devicestorage_t *storage, const char *folder, unsigned int parent_id)
{
	DEBUG(abstract->context, "Garmin/mtp: looking for folder %s under parent id %d", folder, parent_id);
	// memory management is interesting here - we have to always walk the list returned and destroy them one by one
	unsigned int folder_id = LIBMTP_FILES_AND_FOLDERS_ROOT;
	LIBMTP_file_t* files = LIBMTP_Get_Files_And_Folders (device, storage->id, parent_id);
	while (files != NULL) {
		LIBMTP_file_t* mtp_file = files;
		if (mtp_file->filetype == LIBMTP_FILETYPE_FOLDER && mtp_file->filename && !strncasecmp(mtp_file->filename, folder, strlen(folder))) {
			folder_id = mtp_file->item_id;
		}
		files = files->next;
		LIBMTP_destroy_file_t(mtp_file);
	}
	return folder_id;
}

static dc_status_t
mtp_get_file_list(dc_device_t *abstract, struct file_list *files)
{
	garmin_device_t *device = (garmin_device_t *)abstract;
	LIBMTP_raw_device_t *rawdevices;
	int numrawdevices;
	int i;

	LIBMTP_Init();
	DEBUG(abstract->context, "Attempting to connect to mtp device");

	switch (LIBMTP_Detect_Raw_Devices(&rawdevices, &numrawdevices)) {
	case LIBMTP_ERROR_NO_DEVICE_ATTACHED:
		DEBUG(abstract->context, "Garmin/mtp: no device found");
		return DC_STATUS_NODEVICE;
	case LIBMTP_ERROR_CONNECTING:
		DEBUG(abstract->context, "Garmin/mtp: error connecting");
		return DC_STATUS_NOACCESS;
	case LIBMTP_ERROR_MEMORY_ALLOCATION:
		DEBUG(abstract->context, "Garmin/mtp: memory allocation error");
		return DC_STATUS_NOMEMORY;
	case LIBMTP_ERROR_GENERAL: // Unknown general errors - that's bad
	default:
		DEBUG(abstract->context, "Garmin/mtp: unknown error");
		return DC_STATUS_UNSUPPORTED;
	case LIBMTP_ERROR_NONE:
		DEBUG(abstract->context, "Garmin/mtp: successfully connected with %d raw devices", numrawdevices);
	}
	/* iterate through connected MTP devices */
	for (i = 0; i < numrawdevices; i++) {
		LIBMTP_devicestorage_t *storage;
		// we only want to read from a Garmin Descent Mk2 device at this point
		if (rawdevices[i].device_entry.vendor_id != GARMIN_VENDOR ||
		    (rawdevices[i].device_entry.product_id != DESCENT_MK2 && rawdevices[i].device_entry.product_id != DESCENT_MK2_APAC)) {
			DEBUG(abstract->context, "Garmin/mtp: skipping raw device %04x/%04x",
			      rawdevices[i].device_entry.vendor_id, rawdevices[i].device_entry.product_id);
			continue;
		}
		device->mtp_device = LIBMTP_Open_Raw_Device_Uncached(&rawdevices[i]);
		if (device->mtp_device == NULL) {
			DEBUG(abstract->context, "Garmin/mtp: unable to open raw device %d", i);
			continue;
		}
		DEBUG(abstract->context, "Garmin/mtp: succcessfully opened device");
		for (storage = device->mtp_device->storage; storage != 0; storage = storage->next) {
			unsigned int garmin_id = mtp_get_folder_id(abstract, device->mtp_device, storage, "Garmin", LIBMTP_FILES_AND_FOLDERS_ROOT);
			DEBUG(abstract->context, "Garmin/mtp: Garmin folder at file_id %d", garmin_id);
			if (garmin_id == LIBMTP_FILES_AND_FOLDERS_ROOT)
				continue; // this storage partition didn't have a Garmin folder
			unsigned int activity_id = mtp_get_folder_id(abstract, device->mtp_device, storage, "Activity", garmin_id);
			DEBUG(abstract->context, "Garmin/mtp: Activity folder at file_id %d", activity_id);
			if (activity_id == LIBMTP_FILES_AND_FOLDERS_ROOT)
				continue; // no Activity folder

			// now walk that folder to create our file_list
			LIBMTP_file_t* activity_files = LIBMTP_Get_Files_And_Folders (device->mtp_device, storage->id, activity_id);
			while (activity_files != NULL) {
				LIBMTP_file_t* mtp_file = activity_files;
				if (mtp_file->filetype != LIBMTP_FILETYPE_FOLDER && mtp_file->filename) {
					if (check_filename(abstract, mtp_file->filename)) {
						dc_status_t rc = make_space(files);
						if (rc != DC_STATUS_SUCCESS)
							return rc;
						add_name(files, mtp_file->filename, mtp_file->item_id);
					}
				}
				activity_files = activity_files->next;
				LIBMTP_destroy_file_t(mtp_file);
			}
		}
	}
	free(rawdevices);
	DEBUG(abstract->context, "Found %d files", files->nr);

	if (files->array)
		qsort(files->array, files->nr, sizeof(struct fit_file), name_cmp);
	return DC_STATUS_SUCCESS;
}

// MTP hands us the file data in chunks which we then just add to our data buffer
static uint16_t
mtp_put_func(void* params, void* priv, uint32_t sendlen, unsigned char *data, uint32_t *putlen)
{
	dc_buffer_t *file = (dc_buffer_t *)priv;
	dc_buffer_append(file, data, sendlen);
	if (putlen)
		*putlen = sendlen;
	return 0;
}

// read the file from the MTP device and store the content in the data buffer
static dc_status_t
mtp_read_file(garmin_device_t *device, unsigned int file_id, dc_buffer_t *file)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	if (!device->mtp_device) {
		DEBUG(device->base.context, "Garmin/mtp: cannot read file without MTP device");
		return DC_STATUS_NODEVICE;
	}
	DEBUG(device->base.context, "Garmin/mtp: call Get_File_To_Handler");
	if (LIBMTP_Get_File_To_Handler(device->mtp_device, file_id, &mtp_put_func, (void *) file, NULL, NULL) != 0) {
		LIBMTP_Dump_Errorstack(device->mtp_device);
		return DC_STATUS_IO;
	}
	return rc;
}
#endif /* HAVE_LIBMTP */

#ifndef O_BINARY
#define O_BINARY 0
#endif

static dc_status_t
read_file(char *pathname, int pathlen, const char *name, dc_buffer_t *file)
{
	int fd, rc;

	pathname[pathlen] = '/';
	memcpy(pathname+pathlen+1, name, FILE_NAME_SIZE);
	fd = open(pathname, O_RDONLY | O_BINARY);

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
	char pathname[PATH_MAX];
	char pathname_input[PATH_MAX];
	size_t pathlen;
	struct file_list files = {
		0,     // nr
		0,     // allocated
		NULL   // array of file names / ids
	};
	dc_buffer_t *file;
	DIR *dir;
	dc_status_t rc;

	// Read the directory name from the iostream
	rc = dc_iostream_read(device->iostream, &pathname_input, sizeof(pathname_input)-1, &pathlen);
	if (rc != DC_STATUS_SUCCESS)
		return rc;
	pathname_input[pathlen] = 0;

#ifdef HAVE_LIBMTP
	// if the user passes in a path, don't try to read via MTP
	if (pathlen)
		device->use_mtp = 0;
#endif

	// The actual dives are under the "Garmin/Activity/" directory
	// as FIT files, with names like "2018-08-20-10-23-30.fit".
	// Make sure our buffer is big enough.
	if (pathlen + strlen("/Garmin/Activity/") + FILE_NAME_SIZE + 2 > PATH_MAX) {
		ERROR (abstract->context, "Invalid Garmin base directory '%s'", pathname_input);
		return DC_STATUS_IO;
	}
	strcpy(pathname, pathname_input);
	if (pathlen && pathname[pathlen-1] != '/')
		pathname[pathlen++] = '/';
	strcpy(pathname + pathlen, "Garmin/Activity");
	pathlen += strlen("Garmin/Activity");

#ifdef HAVE_LIBMTP
	if (device->use_mtp) {
		rc = mtp_get_file_list(abstract, &files);
		if (rc != DC_STATUS_SUCCESS || !files.nr) {
			free(files.array);
			return rc;
		}
	} else
#endif
	{ // slight coding style violation to deal with the non-MTP case
		dir = opendir(pathname);
		if (!dir) {
			dir = opendir(pathname_input);
			if (!dir) {
				ERROR (abstract->context, "Failed to open directory '%s' or '%s'.", pathname, pathname_input);
				return DC_STATUS_IO;
			}
			strcpy(pathname, pathname_input);
			pathlen = strlen(pathname);
		}
		// Get the list of FIT files
		rc = get_file_list(abstract, dir, &files);
		closedir(dir);
		if (rc != DC_STATUS_SUCCESS || !files.nr) {
			free(files.array);
			return rc;
		}
	}
	// We found at least one file
	// Can we find the fingerprint entry?
	for (int i = 0; i < files.nr; i++) {
		const char *name = files.array[i].name;

		if (memcmp(name, device->fingerprint, sizeof (device->fingerprint)))
			continue;

		// Found fingerprint, just cut the array short here
		files.nr = i;
		DEBUG(abstract->context, "Ignoring '%s' and older", name);
		break;
	}
	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = files.nr;
	progress.current = 0;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	file = dc_buffer_new (16384);
	if (file == NULL) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		free(files.array);
		return DC_STATUS_NOMEMORY;
	}

	dc_event_devinfo_t devinfo;
	dc_event_devinfo_t *devinfo_p = &devinfo;
	for (int i = 0; i < files.nr; i++) {
		const char *name = files.array[i].name;
		dc_parser_t *parser;
		const unsigned char *data;
		unsigned int size;
		short is_dive = 0;

		if (device_is_cancelled(abstract)) {
			status = DC_STATUS_CANCELLED;
			break;
		}

		// Reset the membuffer, read the data
		dc_buffer_clear(file);
		dc_buffer_append(file, name, FIT_NAME_SIZE);
#ifdef HAVE_LIBMTP
		if (device->use_mtp)
			status = mtp_read_file(device, files.array[i].mtp_id, file);
		else
#endif
			status = read_file(pathname, pathlen, name, file);

		if (status != DC_STATUS_SUCCESS)
			break;

		data = dc_buffer_get_data(file);
		size = dc_buffer_get_size(file);

		status = garmin_parser_create(&parser, abstract->context, data, size);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to create parser for dive verification.");
			free(files.array);
			return rc;
		}

		is_dive = !device->model || garmin_parser_is_dive(parser, devinfo_p);
		if (devinfo_p) {
			// first time we came through here, let's emit the
			// devinfo and vendor events
			device_event_emit (abstract, DC_EVENT_DEVINFO, devinfo_p);
			devinfo_p = NULL;
		}
		if (!is_dive) {
			DEBUG(abstract->context, "decided %s isn't a dive.", name);
			dc_parser_destroy(parser);
			continue;
		}

		if (callback && !callback(data, size, name, FIT_NAME_SIZE, userdata))
			break;

		progress.current++;
		device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);
		dc_parser_destroy(parser);
	}

	free(files.array);
	dc_buffer_free(file);
	return status;
}
