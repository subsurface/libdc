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
#include <stdio.h>
#include <string.h>

#include "garmin.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define C_ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

#define MAXFIELDS 128

struct msg_desc;

// Local types
struct type_desc {
	const char *msg_name;
	const struct msg_desc *msg_desc;
	unsigned char nrfields;
	unsigned char fields[MAXFIELDS][3];
};

#define MAXTYPE 16
#define MAXGASES 16
#define MAXSTRINGS 32

typedef struct garmin_parser_t {
	dc_parser_t base;

	dc_sample_callback_t callback;
	void *userdata;

	// Some sample data needs to be bunched up
	// and sent together.
	struct {
		unsigned int time;
		int stop_time;
		double ceiling;
	} sample_data;
	struct type_desc type_desc[MAXTYPE];

	// Field cache
	struct {
		unsigned int initialized;
		unsigned int protocol;
		unsigned int profile;
		unsigned int time;
		int utc_offset, time_offset;
		unsigned int divetime;
		double maxdepth;
		double avgdepth;
		unsigned int ngases;
		dc_gasmix_t gasmix[MAXGASES];
		dc_salinity_t salinity;
		double surface_pressure;
		dc_divemode_t divemode;
		double lowsetpoint;
		double highsetpoint;
		double customsetpoint;
		dc_field_string_t strings[MAXSTRINGS];
		dc_tankinfo_t tankinfo[MAXGASES];
		double tanksize[MAXGASES];
		double tankworkingpressure[MAXGASES];
	} cache;
} garmin_parser_t;

typedef int (*garmin_data_cb_t)(unsigned char type, const unsigned char *data, int len, void *user);

static void flush_pending_sample(struct garmin_parser_t *garmin)
{
	if (!garmin->callback)
		return;

	if (garmin->sample_data.stop_time && garmin->sample_data.ceiling) {
		dc_sample_value_t sample = {0};
		sample.deco.type = DC_DECO_DECOSTOP;
		sample.deco.time = garmin->sample_data.stop_time;
		sample.deco.depth = garmin->sample_data.ceiling;
		garmin->callback(DC_SAMPLE_DECO, sample, garmin->userdata);
	}
	garmin->sample_data.stop_time = 0;
	garmin->sample_data.ceiling = 0;
}


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

#define DECLARE_FIT_TYPE(name, ctype, inval) \
	typedef ctype name;	\
	static const name name##_INVAL = inval

DECLARE_FIT_TYPE(ENUM, unsigned char, 0xff);
DECLARE_FIT_TYPE(UINT8, unsigned char, 0xff);
DECLARE_FIT_TYPE(UINT16, unsigned short, 0xffff);
DECLARE_FIT_TYPE(UINT32, unsigned int, 0xffffffff);
DECLARE_FIT_TYPE(UINT64, unsigned long long, 0xffffffffffffffffull);

DECLARE_FIT_TYPE(UINT8Z, unsigned char, 0);
DECLARE_FIT_TYPE(UINT16Z, unsigned short, 0);
DECLARE_FIT_TYPE(UINT32Z, unsigned int, 0);

DECLARE_FIT_TYPE(SINT8, signed char, 0x7f);
DECLARE_FIT_TYPE(SINT16, signed short, 0x7fff);
DECLARE_FIT_TYPE(SINT32, signed int, 0x7fffffff);
DECLARE_FIT_TYPE(SINT64, signed long long, 0x7fffffffffffffffll);

static const struct {
	const char *type_name;
	int type_size;
	unsigned long long type_inval;
} base_type_info[17] = {
	{ "ENUM",	1, 0xff },
	{ "SINT8",	1, 0x7f },
	{ "UINT8",	1, 0xff },
	{ "SINT16",	2, 0x7fff },
	{ "UINT16",	2, 0xffff },
	{ "SINT32",	4, 0x7fffffff },
	{ "UINT32",	4, 0xffffffff },
	{ "STRING",	1, 0 },
	{ "FLOAT",	4, 0xffffffff },
	{ "DOUBLE",	8, 0xfffffffffffffffful },
	{ "UINT8Z",	1, 0x00 },
	{ "UINT16Z",	2, 0x0000 },
	{ "UINT32Z",	4, 0x00000000 },
	{ "BYTE",	1, 0xff },
	{ "SINT64",	8, 0x7fffffffffffffff },
	{ "UINT64",	8, 0xffffffffffffffff },
	{ "UINT64Z",	8, 0x0000000000000000 },
};

/*
 * Garmin FIT events are described by tuples of "global mesg ID" and
 * a "field number". There's lots of them, because you have events
 * for pretty much anything ("cycling gear change") etc.
 *
 * There's a SDK that generates tables for you, but it looks nasty.
 *
 * So instead, we try to make sense of it manually.
 */
struct field_desc {
	const char *name;
	void (*parse)(struct garmin_parser_t *, unsigned char base_type, const unsigned char *data);
};

#define DECLARE_FIELD(msg, name, type) __DECLARE_FIELD(msg##_##name, type)
#define __DECLARE_FIELD(name, type) \
	static void parse_##name(struct garmin_parser_t *, const type); \
	static void parse_##name##_##type(struct garmin_parser_t *g, unsigned char base_type, const unsigned char *p) \
	{ \
		if (strcmp(#type, base_type_info[base_type].type_name)) \
			fprintf(stderr, "%s: %s should be %s\n", #name, #type, base_type_info[base_type].type_name); \
		type val = *(type *)p; \
		if (val == type##_INVAL) return; \
		DEBUG(g->base.context, "%s (%s): %lld", #name, #type, (long long)val); \
		parse_##name(g, *(type *)p); \
	} \
	static const struct field_desc name##_field_##type = { #name, parse_##name##_##type }; \
	static void parse_##name(struct garmin_parser_t *garmin, type data)

// All msg formats can have a timestamp
// Garmin timestamps are in seconds since 00:00 Dec 31 1989 UTC
// Convert to "standard epoch time" by adding 631065600.
DECLARE_FIELD(ANY, timestamp, UINT32)
{
	if (garmin->callback) {
		dc_sample_value_t sample = {0};

		// Turn the timestamp relative to the beginning of the dive
		if (data < garmin->cache.time)
			return;
		data -= garmin->cache.time;

		// Did we already do this?
		if (data <= garmin->sample_data.time)
			return;

		// Flush any pending sample data before sending the next time event
		flush_pending_sample(garmin);

		// *Now* we're ready to actually update the sample times
		garmin->sample_data.time = data;
		sample.time = data;
		garmin->callback(DC_SAMPLE_TIME, sample, garmin->userdata);
	}
}
DECLARE_FIELD(ANY, message_index, UINT16) { }
DECLARE_FIELD(ANY, part_index, UINT32) { }

// FILE msg
DECLARE_FIELD(FILE, file_type, ENUM) { }
DECLARE_FIELD(FILE, manufacturer, UINT16) { }
DECLARE_FIELD(FILE, product, UINT16) { }
DECLARE_FIELD(FILE, serial, UINT32Z) { }
DECLARE_FIELD(FILE, creation_time, UINT32) { }
DECLARE_FIELD(FILE, number, UINT16) { }
DECLARE_FIELD(FILE, other_time, UINT32) { }

// SESSION msg
DECLARE_FIELD(SESSION, start_time, UINT32) { garmin->cache.time = data; }
DECLARE_FIELD(SESSION, start_pos_lat, SINT32) { }	// 180 deg / 2**31
DECLARE_FIELD(SESSION, start_pos_long, SINT32) { }	// 180 deg / 2**31
DECLARE_FIELD(SESSION, nec_pos_lat, SINT32) { }		// 180 deg / 2**31 NE corner
DECLARE_FIELD(SESSION, nec_pos_long, SINT32) { }	// 180 deg / 2**31    pos
DECLARE_FIELD(SESSION, swc_pos_lat, SINT32) { }		// 180 deg / 2**31 SW corner
DECLARE_FIELD(SESSION, swc_pos_long, SINT32) { }	// 180 deg / 2**31    pos
DECLARE_FIELD(SESSION, exit_pos_lat, SINT32) { }	// 180 deg / 2**31
DECLARE_FIELD(SESSION, exit_pos_long, SINT32) { }	// 180 deg / 2**31

// LAP msg
DECLARE_FIELD(LAP, start_time, UINT32) { }
DECLARE_FIELD(LAP, start_pos_lat, SINT32) { }		// 180 deg / 2**31
DECLARE_FIELD(LAP, start_pos_long, SINT32) { }		// 180 deg / 2**31
DECLARE_FIELD(LAP, end_pos_lat, SINT32) { }		// 180 deg / 2**31
DECLARE_FIELD(LAP, end_pos_long, SINT32) { }		// 180 deg / 2**31
DECLARE_FIELD(LAP, some_pos_lat, SINT32) { }		// 180 deg / 2**31
DECLARE_FIELD(LAP, some_pos_long, SINT32) { }		// 180 deg / 2**31
DECLARE_FIELD(LAP, other_pos_lat, SINT32) { }		// 180 deg / 2**31
DECLARE_FIELD(LAP, other_pos_long, SINT32) { }		// 180 deg / 2**31

// RECORD msg
DECLARE_FIELD(RECORD, position_lat, SINT32) { }		// 180 deg / 2**31
DECLARE_FIELD(RECORD, position_long, SINT32) { }	// 180 deg / 2**31
DECLARE_FIELD(RECORD, altitude, UINT16) { }		// 5 *m + 500 ?
DECLARE_FIELD(RECORD, heart_rate, UINT8) { }		// bpm
DECLARE_FIELD(RECORD, distance, UINT32) { }		// Distance in 100 * m? WTF?
DECLARE_FIELD(RECORD, temperature, SINT8)		// degrees C
{
	if (garmin->callback) {
		dc_sample_value_t sample = {0};
		sample.temperature = data;
		garmin->callback(DC_SAMPLE_TEMPERATURE, sample, garmin->userdata);
	}
}
DECLARE_FIELD(RECORD, abs_pressure, UINT32) {}		// Pascal
DECLARE_FIELD(RECORD, depth, UINT32)			// mm
{
	if (garmin->callback) {
		dc_sample_value_t sample = {0};
		sample.depth = data / 1000.0;
		garmin->callback(DC_SAMPLE_DEPTH, sample, garmin->userdata);
	}
}
DECLARE_FIELD(RECORD, next_stop_depth, UINT32)		// mm
{
	garmin->sample_data.ceiling = data / 1000.0;
}
DECLARE_FIELD(RECORD, next_stop_time, UINT32)		// seconds
{
	garmin->sample_data.stop_time = data;
}
DECLARE_FIELD(RECORD, tts, UINT32) { }			// seconds
DECLARE_FIELD(RECORD, ndl, UINT32)			// s
{
	if (garmin->callback) {
		dc_sample_value_t sample = {0};
		sample.deco.type = DC_DECO_NDL;
		sample.deco.time = data;
		garmin->callback(DC_SAMPLE_DECO, sample, garmin->userdata);
	}
}
DECLARE_FIELD(RECORD, cns_load, UINT8) { }		// percent
DECLARE_FIELD(RECORD, n2_load, UINT16) { }		// percent

// DEVICE_SETTINGS
DECLARE_FIELD(DEVICE_SETTINGS, utc_offset, UINT32) { garmin->cache.utc_offset = (SINT32) data; }	// wrong type in FIT
DECLARE_FIELD(DEVICE_SETTINGS, time_offset, UINT32) { garmin->cache.time_offset = (SINT32) data; }	// wrong type in FIT

// DIVE_GAS - uses msg index
DECLARE_FIELD(DIVE_GAS, helium, UINT8) { } 	// percent
DECLARE_FIELD(DIVE_GAS, oxygen, UINT8) { }	// percent
DECLARE_FIELD(DIVE_GAS, status, ENUM) { }	// 0 - disabled, 1 - enabled, 2 - backup

// DIVE_SUMMARY
DECLARE_FIELD(DIVE_SUMMARY, avg_depth, UINT32) { garmin->cache.avgdepth = data / 1000.0; }		// mm
DECLARE_FIELD(DIVE_SUMMARY, max_depth, UINT32) { garmin->cache.maxdepth = data / 1000.0; }		// mm
DECLARE_FIELD(DIVE_SUMMARY, surface_interval, UINT32) { }	// sec
DECLARE_FIELD(DIVE_SUMMARY, start_cns, UINT8) { }		// percent
DECLARE_FIELD(DIVE_SUMMARY, end_cns, UINT8) { }			// percent
DECLARE_FIELD(DIVE_SUMMARY, start_n2, UINT16) { }		// percent
DECLARE_FIELD(DIVE_SUMMARY, end_n2, UINT16) { }			// percent
DECLARE_FIELD(DIVE_SUMMARY, o2_toxicity, UINT16) { }		// OTUs
DECLARE_FIELD(DIVE_SUMMARY, dive_number, UINT32) { }
DECLARE_FIELD(DIVE_SUMMARY, bottom_time, UINT32) { garmin->cache.divetime = data / 1000; }		// ms


struct msg_desc {
	unsigned char maxfield;
	const struct field_desc *field[];
};

#define SET_FIELD(msg, nr, name, type) \
	[nr] = &msg##_##name##_field_##type

#define DECLARE_MESG(name) \
	static const struct msg_desc name##_msg_desc

DECLARE_MESG(FILE) = {
	.maxfield = 8,
	.field = {
		SET_FIELD(FILE, 0, file_type, ENUM),
		SET_FIELD(FILE, 1, manufacturer, UINT16),
		SET_FIELD(FILE, 2, product, UINT16),
		SET_FIELD(FILE, 3, serial, UINT32Z),
		SET_FIELD(FILE, 4, creation_time, UINT32),
		SET_FIELD(FILE, 5, number, UINT16),
		SET_FIELD(FILE, 7, other_time, UINT32),
	}
};

DECLARE_MESG(DEVICE_SETTINGS) = {
	.maxfield = 3,
	.field = {
		SET_FIELD(DEVICE_SETTINGS, 1, utc_offset, UINT32),	// Convert to UTC
		SET_FIELD(DEVICE_SETTINGS, 2, time_offset, UINT32),	// Convert to local
	}
};
DECLARE_MESG(USER_PROFILE) = { };
DECLARE_MESG(ZONES_TARGET) = { };
DECLARE_MESG(SPORT) = { };

DECLARE_MESG(SESSION) = {
	.maxfield = 40,
	.field = {
		SET_FIELD(SESSION, 2, start_time, UINT32),
		SET_FIELD(SESSION, 3, start_pos_lat, SINT32),	// 180 deg / 2**31
		SET_FIELD(SESSION, 4, start_pos_long, SINT32),	// 180 deg / 2**31
		SET_FIELD(SESSION, 29, nec_pos_lat, SINT32),	// 180 deg / 2**31
		SET_FIELD(SESSION, 30, nec_pos_long, SINT32),	// 180 deg / 2**31
		SET_FIELD(SESSION, 31, swc_pos_lat, SINT32),	// 180 deg / 2**31
		SET_FIELD(SESSION, 32, swc_pos_long, SINT32),	// 180 deg / 2**31
		SET_FIELD(SESSION, 38, exit_pos_lat, SINT32),	// 180 deg / 2**31
		SET_FIELD(SESSION, 39, exit_pos_long, SINT32),	// 180 deg / 2**31
	}
};

DECLARE_MESG(LAP) = {
	.maxfield = 31,
	.field = {
		SET_FIELD(LAP, 2, start_time, UINT32),
		SET_FIELD(LAP, 3, start_pos_lat, SINT32),	// 180 deg / 2**31
		SET_FIELD(LAP, 4, start_pos_long, SINT32),	// 180 deg / 2**31
		SET_FIELD(LAP, 5, end_pos_lat, SINT32),		// 180 deg / 2**31
		SET_FIELD(LAP, 6, end_pos_long, SINT32),	// 180 deg / 2**31
		SET_FIELD(LAP, 27, some_pos_lat, SINT32),	// 180 deg / 2**31
		SET_FIELD(LAP, 28, some_pos_long, SINT32),	// 180 deg / 2**31
		SET_FIELD(LAP, 29, other_pos_lat, SINT32),	// 180 deg / 2**31
		SET_FIELD(LAP, 30, other_pos_long, SINT32),	// 180 deg / 2**31
	}
};

DECLARE_MESG(RECORD) = {
	.maxfield = 99,
	.field = {
		SET_FIELD(RECORD, 0, position_lat, SINT32),	// 180 deg / 2**31
		SET_FIELD(RECORD, 1, position_long, SINT32),	// 180 deg / 2**31
		SET_FIELD(RECORD, 2, altitude, UINT16),		// 5 *m + 500 ?
		SET_FIELD(RECORD, 3, heart_rate, UINT8),	// bpm
		SET_FIELD(RECORD, 5, distance, UINT32),		// Distance in 100 * m? WTF?
		SET_FIELD(RECORD, 13, temperature, SINT8),	// degrees C
		SET_FIELD(RECORD, 91, abs_pressure, UINT32),	// Pascal
		SET_FIELD(RECORD, 92, depth, UINT32),		// mm
		SET_FIELD(RECORD, 93, next_stop_depth, UINT32),	// mm
		SET_FIELD(RECORD, 94, next_stop_time, UINT32),	// seconds
		SET_FIELD(RECORD, 95, tts, UINT32),		// seconds
		SET_FIELD(RECORD, 96, ndl, UINT32),		// s
		SET_FIELD(RECORD, 97, cns_load, UINT8),		// percent
		SET_FIELD(RECORD, 98, n2_load, UINT16),		// percent
	}
};

DECLARE_MESG(DIVE_GAS) = {
	.maxfield = 3,
	.field = {
		// This uses a "message index" field to set the gas index
		SET_FIELD(DIVE_GAS, 0, helium, UINT8),
		SET_FIELD(DIVE_GAS, 1, oxygen, UINT8),
		SET_FIELD(DIVE_GAS, 2, status, ENUM),
	}
};

DECLARE_MESG(DIVE_SUMMARY) = {
	.maxfield = 12,
	.field = {
		SET_FIELD(DIVE_SUMMARY, 2, avg_depth, UINT32),		// mm
		SET_FIELD(DIVE_SUMMARY, 3, max_depth, UINT32),		// mm
		SET_FIELD(DIVE_SUMMARY, 4, surface_interval, UINT32),	// sec
		SET_FIELD(DIVE_SUMMARY, 5, start_cns, UINT8),		// percent
		SET_FIELD(DIVE_SUMMARY, 6, end_cns, UINT8),		// percent
		SET_FIELD(DIVE_SUMMARY, 7, start_n2, UINT16),		// percent
		SET_FIELD(DIVE_SUMMARY, 8, end_n2, UINT16),		// percent
		SET_FIELD(DIVE_SUMMARY, 9, o2_toxicity, UINT16),	// OTUs
		SET_FIELD(DIVE_SUMMARY, 10, dive_number, UINT32),
		SET_FIELD(DIVE_SUMMARY, 11, bottom_time, UINT32),	// ms
	}
};


DECLARE_MESG(EVENT) = { };
DECLARE_MESG(DEVICE_INFO) = { };
DECLARE_MESG(ACTIVITY) = { };
DECLARE_MESG(FILE_CREATOR) = { };
DECLARE_MESG(DIVE_SETTINGS) = { };
DECLARE_MESG(DIVE_ALARM) = { };

// Unknown global message ID's..
DECLARE_MESG(WTF_13) = { };
DECLARE_MESG(WTF_22) = { };
DECLARE_MESG(WTF_79) = { };
DECLARE_MESG(WTF_104) = { };
DECLARE_MESG(WTF_125) = { };
DECLARE_MESG(WTF_140) = { };
DECLARE_MESG(WTF_141) = { };
DECLARE_MESG(WTF_216) = { };
DECLARE_MESG(WTF_233) = { };

#define SET_MESG(nr, name) [nr] = { #name, &name##_msg_desc }

static const struct {
	const char *name;
	const struct msg_desc *desc;
} message_array[] = {
	SET_MESG(  0, FILE),
	SET_MESG(  2, DEVICE_SETTINGS),
	SET_MESG(  3, USER_PROFILE),
	SET_MESG(  7, ZONES_TARGET),
	SET_MESG( 12, SPORT),
	SET_MESG( 13, WTF_13),
	SET_MESG( 18, SESSION),
	SET_MESG( 19, LAP),
	SET_MESG( 20, RECORD),
	SET_MESG( 21, EVENT),
	SET_MESG( 22, WTF_22),
	SET_MESG( 23, DEVICE_INFO),
	SET_MESG( 34, ACTIVITY),
	SET_MESG( 49, FILE_CREATOR),
	SET_MESG( 79, WTF_79),

	SET_MESG(104, WTF_104),
	SET_MESG(125, WTF_125),
	SET_MESG(140, WTF_140),
	SET_MESG(141, WTF_141),

	SET_MESG(216, WTF_216),
	SET_MESG(233, WTF_233),
	SET_MESG(258, DIVE_SETTINGS),
	SET_MESG(259, DIVE_GAS),
	SET_MESG(262, DIVE_ALARM),
	SET_MESG(268, DIVE_SUMMARY),
};

#define MSG_NAME_LEN 16
static const struct msg_desc *lookup_msg_desc(unsigned short msg, int local, const char **namep)
{
	static struct msg_desc local_array[16];
	static char local_name[16][MSG_NAME_LEN];
	struct msg_desc *desc;
	char *name;

	/* Do we have a real one? */
	if (msg < C_ARRAY_SIZE(message_array) && message_array[msg].name) {
		*namep = message_array[msg].name;
		return message_array[msg].desc;
	}

	/* If not, fake it */
	desc = &local_array[local];
	memset(desc, 0, sizeof(*desc));

	name = local_name[local];
	snprintf(name, MSG_NAME_LEN, "msg-%d", msg);
	*namep = name;
	return desc;
}

static int traverse_compressed(struct garmin_parser_t *garmin,
	const unsigned char *data, unsigned int size,
	unsigned char type, unsigned int time)
{
	fprintf(stderr, "Compressed record for local type %d:\n", type);
	return -1;
}

static int traverse_regular(struct garmin_parser_t *garmin,
	const unsigned char *data, unsigned int size,
	unsigned char type, unsigned int *timep)
{
	unsigned int total_len = 0;
	struct type_desc *desc = garmin->type_desc + type;
	const struct msg_desc *msg_desc = desc->msg_desc;
	const char *msg_name = desc->msg_name;

	if (!msg_desc) {
		ERROR(garmin->base.context, "Uninitialized type descriptor %d\n", type);
		return -1;
	}

	for (int i = 0; i < desc->nrfields; i++) {
		const unsigned char *field = desc->fields[i];
		unsigned int field_nr = field[0];
		unsigned int len = field[1];
		unsigned int base_type = field[2] & 0x7f;
		static const int base_size_array[] = { 1, 1, 1, 2, 2, 4, 4, 1, 4, 8, 1, 2, 4, 1, 8, 8, 8 };
		const struct field_desc *field_desc;
		unsigned int base_size;

		if (!len) {
			ERROR(garmin->base.context, "field with zero length\n");
			return -1;
		}

		if (size < len) {
			ERROR(garmin->base.context, "Data traversal size bigger than remaining data (%d vs %d)\n", len, size);
			return -1;
		}

		if (base_type > 16) {
			ERROR(garmin->base.context, "Unknown base type %d\n", base_type);
			data += size;
			len -= size;
			total_len += size;
			continue;
		}
		base_size = base_size_array[base_type];
		if (len % base_size) {
			ERROR(garmin->base.context, "Data traversal size not a multiple of base size (%d vs %d)\n", len, base_size);
			return -1;
		}
		// String
		if (base_type == 7) {
			int string_len = strnlen(data, size);
			if (string_len >= size) {
				ERROR(garmin->base.context, "Data traversal string bigger than remaining data\n");
				return -1;
			}
			if (len <= string_len) {
				ERROR(garmin->base.context, "field length %d, string length %d\n", len, string_len + 1);
				return -1;
			}
		}



		// Certain field numbers have fixed meaning across all messages
		switch (field_nr) {
		case 250:
			field_desc = &ANY_part_index_field_UINT32;
			break;
		case 253:
			field_desc = &ANY_timestamp_field_UINT32;
			break;
		case 254:
			field_desc = &ANY_message_index_field_UINT16;
			break;
		default:
			field_desc = NULL;
			if (field_nr < msg_desc->maxfield)
				field_desc = msg_desc->field[field_nr];
		}

		if (field_desc) {
			field_desc->parse(garmin, base_type, data);
		} else {
			DEBUG(garmin->base.context, "%s/%d %s", msg_name, field_nr, base_type_info[base_type].type_name);
			HEXDUMP(garmin->base.context, DC_LOGLEVEL_DEBUG, "next", data, len);
		}

		data += len;
		total_len += len;
		size -= len;
	}

	return total_len;
}

/*
 * A definition record:
 *
 *  5 bytes of fixed header:
 *	- 1x reserved byte
 *	- 1x architecture byte (0 = LE)
 *	- 2x msg number bytes
 *	- 1x field number byte
 *
 * Followed by the specified number of field definitions:
 *
 *   3 bytes for each field definition:
 *	- 1x "field definition number" (look up in the FIT profile)
 *	- 1x field size in bytes (so you can know the size even if you don't know the definition)
 *	- 1x base type bit field
 *
 * Followed *optionally* by developer definitions (if record header & 0x20):
 *
 *	- 1x number of developer definitions
 *	- 3 bytes each
 */
static int traverse_definition(struct garmin_parser_t *garmin,
	const unsigned char *data, unsigned int size,
	unsigned char record)
{
	unsigned short msg;
	unsigned char type = record & 0xf;
	struct type_desc *desc = garmin->type_desc + type;
	int fields, devfields, len;

	msg = array_uint16_le(data+2);
	desc->msg_desc = lookup_msg_desc(msg, type, &desc->msg_name);
	fields = data[4];

	DEBUG(garmin->base.context, "Define local type %d: %02x %02x %04x %02x %s",
		type, data[0], data[1], msg, fields, desc->msg_name);

	if (data[1]) {
		ERROR(garmin->base.context, "Only handling little-endian definitions\n");
		return -1;
	}

	if (fields > MAXFIELDS) {
		ERROR(garmin->base.context, "Too many fields in description: %d (max %d)\n", fields, MAXFIELDS);
		return -1;
	}
	desc->nrfields = fields;
	len = 5 + fields*3;
	devfields = 0;
	if (record & 0x20) {
		devfields = data[len];
		len += 1 + devfields*3;
		ERROR(garmin->base.context, "NO support for developer fields yet\n");
		return -1;
	}

	for (int i = 0; i < fields; i++) {
		unsigned char *field = desc->fields[i];
		memcpy(field, data + (5+i*3), 3);
		DEBUG(garmin->base.context, "  %d: %02x %02x %02x", i, field[0], field[1], field[2]);
	}

	return len;
}


static dc_status_t
traverse_data(struct garmin_parser_t *garmin)
{
	const unsigned char *data = garmin->base.data;
	int len = garmin->base.size;
	unsigned int hdrsize, protocol, profile, datasize;
	unsigned int time;

	// Reset the time and type descriptors before walking
	memset(&garmin->sample_data, 0, sizeof(garmin->sample_data));
	memset(garmin->type_desc, 0, sizeof(garmin->type_desc));

	// The data starts with our filename fingerprint. Skip it.
	if (len < FIT_NAME_SIZE)
		return DC_STATUS_IO;
	data += FIT_NAME_SIZE;
	len -= FIT_NAME_SIZE;

	// The FIT header
	if (len < 12)
		return DC_STATUS_IO;

	hdrsize = data[0];
	protocol = data[1];
	profile = array_uint16_le(data+2);
	datasize = array_uint32_le(data+4);
	if (memcmp(data+8, ".FIT", 4))
		return DC_STATUS_IO;
	if (hdrsize < 12 || datasize > len || datasize + hdrsize + 2 > len)
		return DC_STATUS_IO;

	garmin->cache.protocol = protocol;
	garmin->cache.profile = profile;

	data += hdrsize;
	time = 0;

	while (datasize > 0) {
		unsigned char record = data[0];
		int len;

		data++;
		datasize--;

		if (record & 0x80) {		// Compressed record?
			unsigned int newtime;
			unsigned char type;

			type = (record >> 5) & 3;
			newtime = (record & 0x1f) | (time & ~0x1f);
			if (newtime < time)
				newtime += 0x20;
			time = newtime;

			len = traverse_compressed(garmin, data, datasize, type, time);
		} else if (record & 0x40) {	// Definition record?
			len = traverse_definition(garmin, data, datasize, record);
		} else {			// Normal data record
			len = traverse_regular(garmin, data, datasize, record, &time);
		}
		if (len <= 0 || len > datasize)
			return DC_STATUS_IO;
		data += len;
		datasize -= len;
	}
	return DC_STATUS_SUCCESS;
}

static dc_status_t
garmin_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	garmin_parser_t *garmin = (garmin_parser_t *) abstract;

	/* Walk the data once without a callback to set up the core fields */
	garmin->callback = NULL;
	garmin->userdata = NULL;
	memset(&garmin->cache, 0, sizeof(garmin->cache));

	traverse_data(garmin);
	flush_pending_sample(garmin);
	return DC_STATUS_SUCCESS;
}


static dc_status_t
garmin_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	garmin_parser_t *garmin = (garmin_parser_t *) abstract;
	dc_ticks_t time = 631065600 + (dc_ticks_t) garmin->cache.time;

	// Show local time (time_offset)
	dc_datetime_gmtime(datetime, time + garmin->cache.time_offset);
	datetime->timezone = DC_TIMEZONE_NONE;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
garmin_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	garmin_parser_t *garmin = (garmin_parser_t *) abstract;

	if (!value)
		return DC_STATUS_INVALIDARGS;

	switch (type) {
	case DC_FIELD_DIVETIME:
		*((unsigned int *) value) = garmin->cache.divetime;
		break;
	case DC_FIELD_AVGDEPTH:
		*((double *) value) = garmin->cache.avgdepth;
		break;
	case DC_FIELD_MAXDEPTH:
		*((double *) value) = garmin->cache.maxdepth;
		break;
	default:
		return DC_STATUS_UNSUPPORTED;
	}
	return DC_STATUS_SUCCESS;
}

static dc_status_t
garmin_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	garmin_parser_t *garmin = (garmin_parser_t *) abstract;

	garmin->callback = callback;
	garmin->userdata = userdata;
	return traverse_data(garmin);
}
