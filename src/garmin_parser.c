/*
 * Garmin Descent Mk1 parsing
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

#include <stdlib.h>

#include "garmin.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

typedef struct garmin_parser_t {
	dc_parser_t base;
} garmin_parser_t;

static dc_status_t garmin_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t garmin_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t garmin_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t garmin_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t garmin_parser_vtable = {
	sizeof(garmin_parser_t),
	DC_FAMILY_GARMIN,
	garmin_parser_set_data, /* set_data */
	garmin_parser_get_datetime, /* datetime */
	garmin_parser_get_field, /* fields */
	garmin_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


dc_status_t
garmin_parser_create (dc_parser_t **out, dc_context_t *context)
{
	garmin_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (garmin_parser_t *) dc_parser_allocate (context, &garmin_parser_vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
garmin_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	return DC_STATUS_SUCCESS;
}


static dc_status_t
garmin_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	const unsigned char *data = abstract->data;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
garmin_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	const unsigned char *data = abstract->data;

	if (!value)
		return DC_STATUS_INVALIDARGS;

	switch (type) {
	case DC_FIELD_DIVETIME:
		*((unsigned int *) value) = 0;
		break;
	case DC_FIELD_AVGDEPTH:
		*((double *) value) = 0;
		break;
	case DC_FIELD_MAXDEPTH:
		*((double *) value) = 0;
		break;
	default:
		return DC_STATUS_UNSUPPORTED;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
garmin_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	return DC_STATUS_SUCCESS;
}
