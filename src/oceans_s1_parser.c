// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2020 Linus Torvalds

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>

#include "oceans_s1.h"
#include "context-private.h"
#include "parser-private.h"
#include "field-cache.h"
#include "array.h"

typedef struct oceans_s1_parser_t oceans_s1_parser_t;

struct oceans_s1_parser_t {
	dc_parser_t base;
	int divenr;
	unsigned int maxdepth, duration;
	long long date;
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
	oceans_s1_parser_t *parser = NULL;

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

static const unsigned char *get_string_line(const unsigned char *in, const unsigned char **next)
{
	const unsigned char *line;
	unsigned char c;

	if (!in) {
		*next = NULL;
		return NULL;
	}

	while (isspace(*in))
		in++;

	if (!*in) {
		*next = NULL;
		return NULL;
	}

	line = in;
	while ((c = *in) != 0) {
		if (c == '\r' || c == '\n')
			break;
		in++;
	}
	*next = in;
	return line;
}

static dc_status_t
oceans_s1_parse_dive(struct oceans_s1_parser_t *s1, const unsigned char *data, dc_sample_callback_t callback, void *userdata)
{
	const unsigned char *line;
	unsigned int sample_interval = 10;
	unsigned int sample_time = 0;

	memset(&s1->cache, 0, sizeof(s1->cache));

	while ((line = get_string_line(data, &data)) != NULL) {
		dc_sample_value_t sample = {0};
		int depth = 0, temp = 0, flags = 0;

		if (!strncmp(line, "divelog ", 8)) {
			sscanf(line, "divelog v1,%us/sample", &sample_interval);
			continue;
		}
		if (!strncmp(line, "dive ", 5)) {
			int nr, unknown, o2;
			long long date;

			sscanf(line, "dive %d,%d,%d,%lld", &nr, &unknown, &o2, &date);
			s1->divenr = nr;
			s1->date = date;
			// I think "unknown" is dive mode
			if (o2) {
				dc_gasmix_t mix = { 0 };
				mix.oxygen = o2 / 100.0;
				DC_ASSIGN_FIELD(s1->cache, GASMIX_COUNT, 1);
				DC_ASSIGN_IDX(s1->cache, GASMIX, 0, mix);
			}
			continue;
		}
		if (!strncmp(line, "continue ", 9)) {
			int depth = 0, seconds = 0;
			sscanf(line, "continue %d,%d", &depth, &seconds);

			// Create surface samples for the surface time,
			// and then a depth sample at the stated depth
			if (callback) {
				if (seconds >= sample_interval*2) {
					dc_sample_value_t sample = {0};
					sample.time = sample_time + sample_interval;
					callback(DC_SAMPLE_TIME, sample, userdata);
					sample.depth = 0;
					callback(DC_SAMPLE_DEPTH, sample, userdata);

					sample.time = sample_time + seconds - sample_interval;
					callback(DC_SAMPLE_TIME, sample, userdata);
					sample.depth = 0;
					callback(DC_SAMPLE_DEPTH, sample, userdata);
				}
				sample.time = sample_time + seconds;
				callback(DC_SAMPLE_TIME, sample, userdata);
				sample.depth = depth / 100.0;
				callback(DC_SAMPLE_DEPTH, sample, userdata);
			}
			sample_time += seconds;
			continue;
		}
		if (!strncmp(line, "enddive ", 8)) {
			int maxdepth = 0, duration = 0;
			sscanf(line, "enddive %d,%d", &maxdepth, &duration);
			DC_ASSIGN_FIELD(s1->cache, MAXDEPTH, maxdepth / 100.0);
			DC_ASSIGN_FIELD(s1->cache, DIVETIME, duration);
			s1->maxdepth = maxdepth;
			s1->duration = duration;
			continue;
		}
		if (sscanf(line, "%d,%d,%d", &depth, &temp, &flags) != 3)
			continue;

		sample_time += sample_interval;
		if (callback) {
			dc_sample_value_t sample = {0};
			sample.time = sample_time;
			callback(DC_SAMPLE_TIME, sample, userdata);
			sample.depth = depth / 100.0;
			callback(DC_SAMPLE_DEPTH, sample, userdata);
			sample.temperature = temp;
			callback (DC_SAMPLE_TEMPERATURE, sample, userdata);
		}
	}
	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceans_s1_parser_set_data(dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	struct oceans_s1_parser_t *s1 = (struct oceans_s1_parser_t *)abstract;

	return oceans_s1_parse_dive(s1, data, NULL, NULL);
}

static dc_status_t
oceans_s1_parser_get_datetime(dc_parser_t *abstract, dc_datetime_t *datetime)
{
	oceans_s1_parser_t *s1 = (oceans_s1_parser_t *)abstract;

	dc_datetime_gmtime(datetime, s1->date);
	return DC_STATUS_SUCCESS;
}

static dc_status_t
oceans_s1_parser_get_field(dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	oceans_s1_parser_t *s1 = (oceans_s1_parser_t *)abstract;

	return dc_field_get(&s1->cache, type, flags, value);
}

static dc_status_t
oceans_s1_parser_samples_foreach(dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	struct oceans_s1_parser_t *s1 = (struct oceans_s1_parser_t *)abstract;

	return oceans_s1_parse_dive(s1, s1->base.data, callback, userdata);
}
