/*
 * Deeplu Cosmiq+ parsing
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

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "deepblu.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define C_ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

#define MAXFIELDS 128

struct msg_desc;

#define MAXTYPE 16
#define MAXGASES 16
#define MAXSTRINGS 32

typedef struct deepblu_parser_t {
	dc_parser_t base;

	dc_sample_callback_t callback;
	void *userdata;

	// Field cache
	struct {
		unsigned int initialized;

		// dc_get_field() data
		unsigned int DIVETIME;
		double MAXDEPTH;
		double AVGDEPTH;
		unsigned int GASMIX_COUNT;
		dc_salinity_t SALINITY;
		dc_gasmix_t gasmix[MAXGASES];

		dc_field_string_t strings[MAXSTRINGS];
	} cache;
} deepblu_parser_t;

// I *really* need to make this generic
static void add_string(deepblu_parser_t *deepblu, const char *desc, const char *data);
static void add_string_fmt(deepblu_parser_t *deepblu, const char *desc, const char *fmt, ...);

/*
 * Macro to make it easy to set DC_FIELD_xyz values
 */
#define ASSIGN_FIELD(name, value) do { \
	deepblu->cache.initialized |= 1u << DC_FIELD_##name; \
	deepblu->cache.name = (value); \
} while (0)


static dc_status_t deepblu_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t deepblu_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t deepblu_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t deepblu_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t deepblu_parser_vtable = {
	sizeof(deepblu_parser_t),
	DC_FAMILY_DEEPBLU,
	deepblu_parser_set_data, /* set_data */
	deepblu_parser_get_datetime, /* datetime */
	deepblu_parser_get_field, /* fields */
	deepblu_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

dc_status_t
deepblu_parser_create (dc_parser_t **out, dc_context_t *context)
{
	deepblu_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (deepblu_parser_t *) dc_parser_allocate (context, &deepblu_parser_vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	*out = (dc_parser_t *) parser;

	ERROR (context, "Deepblu Cosmiq+ parser_create() called");
	return DC_STATUS_SUCCESS;
}

/*
 * FIXME! This should all be generic.
 *
 * Now it's just copied between all the different
 * dive computers that support the strings..
 */
static void add_string(deepblu_parser_t *deepblu, const char *desc, const char *value)
{
	int i;

	deepblu->cache.initialized |= 1 << DC_FIELD_STRING;
	for (i = 0; i < MAXSTRINGS; i++) {
		dc_field_string_t *str = deepblu->cache.strings+i;
		if (str->desc)
			continue;
		str->desc = desc;
		str->value = strdup(value);
		break;
	}
}

static void add_string_fmt(deepblu_parser_t *deepblu, const char *desc, const char *fmt, ...)
{
	char buffer[256];
	va_list ap;

	va_start(ap, fmt);
	buffer[sizeof(buffer)-1] = 0;
	(void) vsnprintf(buffer, sizeof(buffer)-1, fmt, ap);
	va_end(ap);

	add_string(deepblu, desc, buffer);
}

static dc_status_t
deepblu_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	deepblu_parser_t *deepblu = (deepblu_parser_t *) abstract;

	deepblu->callback = NULL;
	deepblu->userdata = NULL;
	memset(&deepblu->cache, 0, sizeof(deepblu->cache));

	ERROR (abstract->context, "Deepblu Cosmiq+ parser_set_data() called");
	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepblu_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	deepblu_parser_t *deepblu = (deepblu_parser_t *) abstract;

	ERROR (abstract->context, "Deepblu Cosmiq+ parser_get_datetime() called");
	return DC_STATUS_UNSUPPORTED;
}

static dc_status_t get_string_field(dc_field_string_t *strings, unsigned idx, dc_field_string_t *value)
{
	if (idx < MAXSTRINGS) {
		dc_field_string_t *res = strings+idx;
		if (res->desc && res->value) {
			*value = *res;
			return DC_STATUS_SUCCESS;
		}
	}
	return DC_STATUS_UNSUPPORTED;
}

// Ugly define thing makes the code much easier to read
// I'd love to use __typeof__, but that's a gcc'ism
#define field_value(p, NAME) \
	(memcpy((p), &deepblu->cache.NAME, sizeof(deepblu->cache.NAME)), DC_STATUS_SUCCESS)
// Hacky hack hack
#define GASMIX gasmix[flags]

static dc_status_t
deepblu_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	deepblu_parser_t *deepblu = (deepblu_parser_t *) abstract;

	if (!value)
		return DC_STATUS_INVALIDARGS;

	/* This whole sequence should be standardized */
	if (!(deepblu->cache.initialized & (1 << type)))
		return DC_STATUS_UNSUPPORTED;

	switch (type) {
	case DC_FIELD_DIVETIME:
		return field_value(value, DIVETIME);
	case DC_FIELD_MAXDEPTH:
		return field_value(value, MAXDEPTH);
	case DC_FIELD_AVGDEPTH:
		return field_value(value, AVGDEPTH);
	case DC_FIELD_GASMIX_COUNT:
	case DC_FIELD_TANK_COUNT:
		return field_value(value, GASMIX_COUNT);
	case DC_FIELD_GASMIX:
		if (flags >= MAXGASES)
			return DC_STATUS_UNSUPPORTED;
		return field_value(value, GASMIX);
	case DC_FIELD_SALINITY:
		return field_value(value, SALINITY);
	case DC_FIELD_ATMOSPHERIC:
		return DC_STATUS_UNSUPPORTED;
	case DC_FIELD_DIVEMODE:
		return DC_STATUS_UNSUPPORTED;
	case DC_FIELD_TANK:
		return DC_STATUS_UNSUPPORTED;
	case DC_FIELD_STRING:
		return get_string_field(deepblu->cache.strings, flags, (dc_field_string_t *)value);
	default:
		return DC_STATUS_UNSUPPORTED;
	}
	return DC_STATUS_SUCCESS;
}

static dc_status_t
deepblu_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	deepblu_parser_t *deepblu = (deepblu_parser_t *) abstract;

	deepblu->callback = callback;
	deepblu->userdata = userdata;

	ERROR (abstract->context, "Deepblu Cosmiq+ samples_foreach() called");
	return DC_STATUS_SUCCESS;
}
