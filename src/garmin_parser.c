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
	struct type_desc type_desc[MAXTYPE];
	// Field cache
	struct {
		unsigned int initialized;
		unsigned int protocol;
		unsigned int profile;
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
	int (*parse)(struct garmin_parser_t *, const unsigned char *data);
};

#define DECLARE_FIELD(msg, name, type) __DECLARE_FIELD(msg##_##name, type)
#define __DECLARE_FIELD(name, type) \
	static int parse_##name(struct garmin_parser_t *, const type); \
	static int parse_##name##_##type(struct garmin_parser_t *g, const unsigned char *p) \
	{ \
		type val = *(type *)p; \
		if (val == type##_INVAL) return 0; \
		fprintf(stderr, "%s: %llx\n", #name, (long long)val); \
		return parse_##name(g, *(type *)p); \
	} \
	static const struct field_desc name##_field_##type = { #name, parse_##name##_##type }; \
	static int parse_##name(struct garmin_parser_t *garmin, type data)

// All msg formats can have a timestamp
// Garmin timestamps are in seconds since 00:00 Dec 31 1989 UTC
// Convert to "standard epoch time" by adding 631065600.
DECLARE_FIELD(ANY, timestamp, UINT32)
{
	dc_ticks_t time = 631065600 + (dc_ticks_t) data;
	dc_datetime_t date;

	dc_datetime_gmtime(&date, time);
	fprintf(stderr, "%04d-%02d-%02d %2d:%02d:%02d\n",
		date.year, date.month, date.day,
		date.hour, date.minute, date.second);
	return 0;
}
DECLARE_FIELD(ANY, message_index, UINT16) { return 0; }
DECLARE_FIELD(ANY, part_index, UINT32) { return 0; }

// FILE msg
DECLARE_FIELD(FILE, file_type, ENUM) { return 0; }
DECLARE_FIELD(FILE, manufacturer, UINT16) { return 0; }
DECLARE_FIELD(FILE, product, UINT16) { return 0; }
DECLARE_FIELD(FILE, serial, UINT32) { return 0; }
DECLARE_FIELD(FILE, creation_time, UINT32) { return parse_ANY_timestamp(garmin, data); }
DECLARE_FIELD(FILE, number, UINT16) { return 0; }
DECLARE_FIELD(FILE, other_time, UINT32) { return parse_ANY_timestamp(garmin, data); }

// SESSION msg
DECLARE_FIELD(SESSION, start_time, UINT32) { return parse_ANY_timestamp(garmin, data); }

// LAP msg
DECLARE_FIELD(LAP, start_time, UINT32) { return parse_ANY_timestamp(garmin, data); }

// RECORD msg
DECLARE_FIELD(RECORD, start_time, UINT32) { return parse_ANY_timestamp(garmin, data); }


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
		SET_FIELD(FILE, 3, serial, UINT32),
		SET_FIELD(FILE, 4, creation_time, UINT32),
		SET_FIELD(FILE, 5, number, UINT16),
		SET_FIELD(FILE, 7, other_time, UINT32),
	}
};

DECLARE_MESG(DEVICE_SETTINGS) = { };
DECLARE_MESG(USER_PROFILE) = { };
DECLARE_MESG(ZONES_TARGET) = { };
DECLARE_MESG(SPORT) = { };

DECLARE_MESG(SESSION) = {
	.maxfield = 3,
	.field = {
		SET_FIELD(SESSION, 2, start_time, UINT32),
	}
};

DECLARE_MESG(LAP) = {
	.maxfield = 3,
	.field = {
		SET_FIELD(LAP, 2, start_time, UINT32),
	}
};

DECLARE_MESG(RECORD) = {
	.maxfield = 3,
	.field = {
		SET_FIELD(RECORD, 2, start_time, UINT32),
	}
};

DECLARE_MESG(EVENT) = { };
DECLARE_MESG(DEVICE_INFO) = { };
DECLARE_MESG(ACTIVITY) = { };
DECLARE_MESG(FILE_CREATOR) = { };
DECLARE_MESG(DIVE_SETTINGS) = { };
DECLARE_MESG(DIVE_GAS) = { };
DECLARE_MESG(DIVE_ALARM) = { };
DECLARE_MESG(DIVE_SUMMARY) = { };

// Unknown global message ID's..
DECLARE_MESG(WTF_13) = { };
DECLARE_MESG(WTF_22) = { };
DECLARE_MESG(WTF_79) = { };
DECLARE_MESG(WTF_104) = { };
DECLARE_MESG(WTF_125) = { };
DECLARE_MESG(WTF_140) = { };
DECLARE_MESG(WTF_141) = { };
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
			field_desc->parse(garmin, data);
		} else {
#if 1
			fprintf(stderr, "%s/%d:", msg_name, field_nr);
			if (base_type == 7)
				fprintf(stderr, " %s\n", data);
			else {
				for (int i = 0; i < len; i += base_size) {
					unsigned long long value;
					const char *fmt;
					const unsigned char *ptr = data + i;
					switch (base_size) {
					default: value = *ptr; fmt = " %02llx"; break;
					case 2: value = *(unsigned short *)ptr; fmt = " %04llx"; break;
					case 4: value = *(unsigned int *)ptr; fmt = " %08llx"; break;
					case 8: value = *(unsigned long long *)ptr; fmt = " %016llx"; break;
					}
					fprintf(stderr, fmt, value);
				}
				fprintf(stderr, "\n");
			}
#else
			DEBUG(garmin->base.context, "Unknown field %s:%02x %02x %d/%d\n", msg_name, field_nr, field[2], len, base_size);
	                HEXDUMP(garmin->base.context, DC_LOGLEVEL_DEBUG, "next", data, len);
#endif
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


static int traverse_data(struct garmin_parser_t *garmin)
{
	const unsigned char *data = garmin->base.data;
	int len = garmin->base.size;
	unsigned int hdrsize, protocol, profile, datasize;
	unsigned int time;

	// The data starts with our filename fingerprint. Skip it.
	data += FIT_NAME_SIZE;
	len -= FIT_NAME_SIZE;

	// The FIT header
	if (len < 12)
		return -1;

	hdrsize = data[0];
	protocol = data[1];
	profile = array_uint16_le(data+2);
	datasize = array_uint32_le(data+4);
	if (memcmp(data+8, ".FIT", 4))
		return -1;
	if (hdrsize < 12 || datasize > len || datasize + hdrsize + 2 > len)
		return -1;

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
			return -1;
		data += len;
		datasize -= len;
	}
	return 0;
}

static void initialize_field_caches(garmin_parser_t *garmin)
{
	memset(&garmin->cache, 0, sizeof(garmin->cache));
	traverse_data(garmin);
}

static dc_status_t
garmin_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	garmin_parser_t *garmin = (garmin_parser_t *) abstract;

	memset(garmin->type_desc, 0, sizeof(garmin->type_desc));
	initialize_field_caches(garmin);
	return DC_STATUS_SUCCESS;
}


static dc_status_t
garmin_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	const unsigned char *data = abstract->data;
	unsigned int yyyy, mm, dd, h, m, s;

	if (abstract->size < FIT_NAME_SIZE)
		return DC_STATUS_UNSUPPORTED;

	if (sscanf(data, "%04u-%02u-%02u-%02u-%02u-%02u",
		   &yyyy, &mm, &dd, &h, &m, &s) != 6)
		return DC_STATUS_UNSUPPORTED;

	datetime->year     = yyyy;
	datetime->month    = mm;
	datetime->day      = dd;
	datetime->hour     = h;
	datetime->minute   = m;
	datetime->second   = s;
	datetime->timezone = DC_TIMEZONE_NONE;

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
