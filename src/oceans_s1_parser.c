// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2020 Linus Torvalds

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "oceans_s1.h"
#include "context-private.h"
#include "parser-private.h"
#include "field-cache.h"
#include "array.h"

typedef struct oceans_s1_parser_t oceans_s1_parser_t;

struct oceans_s1_parser_t {
	dc_parser_t base;
	struct dc_field_cache cache;
};

static dc_status_t oceans_s1_parser_set_data(dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t oceans_s1_parser_get_datetime(dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t oceans_s1_parser_get_field(dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t oceans_s1_parser_samples_foreach(dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t oceans_s1_parser_vtable = {
	sizeof(oceans_s1_parser_t),
	DC_FAMILY_OCEANS_S1,
	oceans_s1_parser_set_data, /* set_data */
	oceans_s1_parser_get_datetime, /* datetime */
	oceans_s1_parser_get_field, /* fields */
	oceans_s1_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

dc_status_t
oceans_s1_parser_create(dc_parser_t **out, dc_context_t *context)
{
	oceans_s1_parser_t* parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (oceans_s1_parser_t*)dc_parser_allocate(context, &oceans_s1_parser_vtable);
	if (parser == NULL) {
		ERROR(context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	*out = (dc_parser_t*)parser;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceans_s1_parser_set_data(dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	// Fill me
	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceans_s1_parser_get_datetime(dc_parser_t* abstract, dc_datetime_t* datetime)
{
	// Fill me
	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceans_s1_parser_get_field(dc_parser_t* abstract, dc_field_type_t type, unsigned int flags, void* value)
{
	oceans_s1_parser_t *s1 = (oceans_s1_parser_t *)abstract;

	return dc_field_get(&s1->cache, type, flags, value);
}

static dc_status_t
oceans_s1_parser_samples_foreach(dc_parser_t* abstract, dc_sample_callback_t callback, void* userdata)
{
	// Fill me
	return DC_STATUS_SUCCESS;
}
