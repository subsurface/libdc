// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2020 Linus Torvalds

#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <stdint.h>

#include "oceans_s1.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &oceans_s1_device_vtable)

typedef struct oceans_s1_device_t {
	dc_device_t base;
	dc_iostream_t* iostream;
	unsigned char fingerprint[4];
} oceans_s1_device_t;

static dc_status_t oceans_s1_device_set_fingerprint(dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t oceans_s1_device_foreach(dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t oceans_s1_device_close(dc_device_t *abstract);

static const dc_device_vtable_t oceans_s1_device_vtable = {
	sizeof(oceans_s1_device_t),
	DC_FAMILY_OCEANS_S1,
	oceans_s1_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	oceans_s1_device_foreach, /* foreach */
	NULL, /* timesync */
	oceans_s1_device_close, /* close */
};

dc_status_t
oceans_s1_device_open(dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	char buffer[128];
	dc_status_t status = DC_STATUS_SUCCESS;
	oceans_s1_device_t *s1 = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	s1 = (oceans_s1_device_t*)dc_device_allocate(context, &oceans_s1_device_vtable);
	if (s1 == NULL) {
		ERROR(context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	s1->iostream = iostream;
	memset(s1->fingerprint, 0, sizeof(s1->fingerprint));

	*out = (dc_device_t*)s1;

	// Fill in

	return DC_STATUS_IO;
}

static dc_status_t
oceans_s1_device_close(dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceans_s1_device_t *s1 = (oceans_s1_device_t*)abstract;

	// Fill in

	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceans_s1_device_set_fingerprint(dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	oceans_s1_device_t *s1 = (oceans_s1_device_t*)abstract;

	if (size && size != sizeof(s1->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy(s1->fingerprint, data, sizeof(s1->fingerprint));
	else
		memset(s1->fingerprint, 0, sizeof(s1->fingerprint));

	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceans_s1_device_foreach(dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceans_s1_device_t *s1 = (oceans_s1_device_t*)abstract;

	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

	progress.current = 0;
	progress.maximum = 0;
	device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

	// Fill in

	return status;
}
