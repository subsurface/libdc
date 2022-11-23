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
#include <stdarg.h>
#include <string.h>

#include "garmin.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"
#include "field-cache.h"

#define MAXFIELDS 128

struct msg_desc;

// Local types
struct type_desc {
	const char *msg_name;
	const struct msg_desc *msg_desc;
	unsigned char nrfields;
	unsigned char fields[MAXFIELDS][3];
};

// Positions are signed 32-bit values, turning
// into 180 * val // 2**31 degrees.
struct pos {
	int lat, lon;
};

#define MAX_SENSORS 6
struct garmin_sensor {
	unsigned int sensor_id;
	const char *sensor_name;
	unsigned char sensor_enabled, sensor_units, sensor_used_for_gas_rate;
	unsigned int sensor_rated_pressure, sensor_reserve_pressure, sensor_volume;
};

#define MAXTYPE 16
#define MAXGASES 16
#define MAXSTRINGS 32

// Some record data needs to be bunched up
// and sent together.
struct record_data {
	unsigned int pending;
	unsigned int time;

	// RECORD_DECO
	int stop_time;
	double ceiling;

	// RECORD_GASMIX
	int index, gas_status;
	dc_gasmix_t gasmix;

	// RECORD_EVENT
	unsigned char event_type, event_nr, event_group;
	unsigned int event_data, event_unknown;

	// RECORD_DEVICE_INFO
	unsigned int device_index, firmware, serial, product;

	// RECORD_DECO_MODEL
	unsigned char model, gf_low, gf_high;

	// RECORD_SENSOR_PROFILE has no data, fills in dive.sensor[nr_sensor]

	// RECORD_TANK_UPDATE
	unsigned int sensor, pressure;

	// RECORD_SETPOINT_CHANGE
	unsigned int setpoint_actual_cbar;
};

#define RECORD_GASMIX		1
#define RECORD_DECO		2
#define RECORD_EVENT		4
#define RECORD_DEVICE_INFO	8
#define RECORD_DECO_MODEL	16
#define RECORD_SENSOR_PROFILE	32
#define RECORD_TANK_UPDATE	64
#define RECORD_SETPOINT_CHANGE	128

typedef struct garmin_parser_t {
	dc_parser_t base;

	dc_sample_callback_t callback;
	void *userdata;

	// Multi-value record data
	struct record_data record_data;

	struct type_desc type_desc[MAXTYPE];

	// Field cache
	struct {
		unsigned int sub_sport;
		unsigned int serial;
		unsigned int product;
		unsigned int firmware;
		unsigned int protocol;
		unsigned int profile;
		unsigned int time;
		int utc_offset, time_offset;
		unsigned int nr_sensor;
		struct garmin_sensor sensor[MAX_SENSORS];
		unsigned int setpoint_low_cbar, setpoint_high_cbar;
		unsigned int setpoint_low_switch_depth_mm, setpoint_high_switch_depth_mm;
	} dive;

	// I count nine (!) different GPS fields Hmm.
	// Reporting all of them just to try to figure
	// out what is what.
	struct {
		struct {
			struct pos entry, exit;
			struct pos NE, SW; // NE, SW corner
		} SESSION;
		struct {
			struct pos entry, exit;
			struct pos some, other;
		} LAP;
		struct pos RECORD;
	} gps;

	struct dc_field_cache cache;
	unsigned char is_big_endian; // instead of bool
} garmin_parser_t;

typedef int (*garmin_data_cb_t)(unsigned char type, const unsigned char *data, int len, void *user);

static inline struct garmin_sensor *current_sensor(garmin_parser_t *garmin)
{
	return garmin->dive.sensor + garmin->dive.nr_sensor;
}

static int find_tank_index(garmin_parser_t *garmin, unsigned int sensor_id)
{
	for (int i = 0; i < garmin->dive.nr_sensor; i++) {
		if (garmin->dive.sensor[i].sensor_id == sensor_id)
			return i;
	}
	return 0;
}

/*
 * Decode the event. Numbers from Wojtek's fit2subs python script
 */
static void garmin_event(struct garmin_parser_t *garmin,
		unsigned char event, unsigned char type, unsigned char group,
		unsigned int data, unsigned int unknown)
{
	static const struct {
		// 1 - state, 2 - notify, 3 - warning, 4 - alarm
		int severity;
		const char *name;
	} event_desc[] = {
		[0] =  { 2, "Deco required" },
		[1] =  { 2, "Gas Switch prompted" },
		[2] =  { 1, "Surface" },
		[3] =  { 2, "Approaching NDL" },
		[4] =  { 3, "ppO2 warning" },
		[5] =  { 4, "ppO2 critical high" },
		[6] =  { 4, "ppO2 critical low" },
		[7] =  { 2, "Time alert" },
		[8] =  { 2, "Depth alert" },
		[9] =  { 3, "Deco ceiling broken" },
		[10] = { 1, "Deco completed" },
		[11] = { 3, "Safety stop ceiling broken" },
		[12] = { 1, "Safety stop completed" },
		[13] = { 3, "CNS warning" },
		[14] = { 4, "CNS critical" },
		[15] = { 3, "OTU warning" },
		[16] = { 4, "OTU critical" },
		[17] = { 3, "Ascent speed critical" },
		[18] = { 1, "Alert dismissed" },
		[19] = { 1, "Alert timed out" },
		[20] = { 3, "Battry Low" },
		[21] = { 3, "Battry Critical" },
		[22] = { 1, "Safety stop begin" },
		[23] = { 1, "Approaching deco stop" },
		[24] = { 1, "Switched to low setpoint" },
		[25] = { 1, "Switched to high setpoint" },
		[32] = { 1, "Tank battery low" },	// No way to know which tank
	};
	dc_sample_value_t sample = {0};

	switch (event) {
	case 38:
		break;
	case 48:
		break;
	case 56:
		if (data >= C_ARRAY_SIZE(event_desc))
			return;

		sample.event.type = SAMPLE_EVENT_STRING;
		sample.event.name = event_desc[data].name;
		sample.event.flags =  event_desc[data].severity << SAMPLE_FLAGS_SEVERITY_SHIFT;

		if (data == 24 || data == 25) {
			// Update the actual setpoint used during the dive and report it
			garmin->record_data.setpoint_actual_cbar = data == 24 ? garmin->dive.setpoint_low_cbar : garmin->dive.setpoint_high_cbar;
			garmin->record_data.pending |= RECORD_SETPOINT_CHANGE;
		}

		if (!sample.event.name)
			return;
		garmin->callback(DC_SAMPLE_EVENT, sample, garmin->userdata);
		return;

	case 57:
		sample.gasmix = data;
		garmin->callback(DC_SAMPLE_GASMIX, sample, garmin->userdata);
		return;
	}
}

/*
 * Some data isn't just something we can save off directly: it's a record with
 * multiple fields where one field describes another.
 *
 * The solution is to just batch it up in the "garmin->record_data", and then
 * this function gets called at the end of a record.
 */
static void flush_pending_record(struct garmin_parser_t *garmin)
{
	struct record_data *record = &garmin->record_data;
	unsigned int pending = record->pending;

	record->pending = 0;
	if (!garmin->callback) {
		if (pending & RECORD_GASMIX) {
			// 0 - disabled, 1 - enabled, 2 - backup
			int enabled = record->gas_status > 0;
			int index = record->index;
			if (enabled && index < MAXGASES) {
				DC_ASSIGN_IDX(garmin->cache, GASMIX, index, record->gasmix);
				DC_ASSIGN_FIELD(garmin->cache, GASMIX_COUNT, index+1);
			}
		}
		if (pending & RECORD_DEVICE_INFO && record->device_index == 0) {
			garmin->dive.firmware = record->firmware;
			garmin->dive.serial = record->serial;
			garmin->dive.product = record->product;
		}
		if (pending & RECORD_DECO_MODEL)
			dc_field_add_string_fmt(&garmin->cache, "Deco model", "Buhlmann ZHL-16C %u/%u", record->gf_low, record->gf_high);

		// End of sensor record just increments nr_sensor,
		// so that the next sensor record will start
		// filling in the next one.
		//
		// NOTE! This only happens for tank pods, other
		// sensors will just overwrite each other.
		//
		// Also note that the last sensor is just for
		// scratch use, so that the sensor record can
		// always fill in dive.sensor[nr_sensor] with
		// no checking.
		if (pending & RECORD_SENSOR_PROFILE) {
			if (garmin->dive.nr_sensor < MAX_SENSORS-1)
				garmin->dive.nr_sensor++;
		}

		return;
	}

	if (pending &  RECORD_DECO) {
		dc_sample_value_t sample = {0};
		sample.deco.type = DC_DECO_DECOSTOP;
		sample.deco.time = record->stop_time;
		sample.deco.depth = record->ceiling;
		garmin->callback(DC_SAMPLE_DECO, sample, garmin->userdata);
	}

	if (pending & RECORD_EVENT) {
		garmin_event(garmin, record->event_nr, record->event_type,
			record->event_group, record->event_data, record->event_unknown);
	}

	if (pending & RECORD_TANK_UPDATE) {
		dc_sample_value_t sample = {0};

		sample.pressure.tank = find_tank_index(garmin, record->sensor);
		sample.pressure.value = record->pressure / 100.0;
		garmin->callback(DC_SAMPLE_PRESSURE, sample, garmin->userdata);
	}

	if (pending & RECORD_SETPOINT_CHANGE) {
		dc_sample_value_t sample = {0};

		sample.setpoint = record->setpoint_actual_cbar / 100.0;
		garmin->callback(DC_SAMPLE_SETPOINT, sample, garmin->userdata);
	}
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

DECLARE_FIT_TYPE(FLOAT, unsigned int, 0xffffffff);
DECLARE_FIT_TYPE(DOUBLE, unsigned long long, 0xffffffffffffffffll);
DECLARE_FIT_TYPE(STRING, char *, NULL);

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

static inline int base_type_is_integer(unsigned char base_type)
{
	return !memcmp(base_type_info[base_type].type_name + 1, "INT", 3);
}

static inline unsigned int array_uint_endian(const unsigned char *p, unsigned int type_size, unsigned char bigendian)
{
	if (bigendian)
		return array_uint_be(p, type_size);
	else
		return array_uint_le(p, type_size);
}

#define DECLARE_FIELD(msg, name, type) __DECLARE_FIELD(msg##_##name, type)
#define __DECLARE_FIELD(name, type) \
	static void parse_##name(struct garmin_parser_t *, const type); \
	static void parse_##name##_##type(struct garmin_parser_t *g, unsigned char base_type, const unsigned char *p) \
	{ \
		if (strcmp(#type, base_type_info[base_type].type_name)) \
			fprintf(stderr, "%s: %s should be %s\n", #name, #type, base_type_info[base_type].type_name); \
		type val; \
		if (base_type_info[base_type].type_size > 1 && base_type_is_integer(base_type)) \
			val = (type)array_uint_endian(p, base_type_info[base_type].type_size, g->is_big_endian); \
		else \
			val = *(type *)p; \
		if (val == type##_INVAL) return; \
		DEBUG(g->base.context, "%s (%s): %lld", #name, #type, (long long)val); \
		parse_##name(g, val); \
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
		if (data < garmin->dive.time)
			return;
		data -= garmin->dive.time;

		// Did we already do this?
		if (data < garmin->record_data.time)
			return;

		// Now we're ready to actually update the sample times
		garmin->record_data.time = data+1;
		sample.time = data;
		garmin->callback(DC_SAMPLE_TIME, sample, garmin->userdata);
	}
}
DECLARE_FIELD(ANY, message_index, UINT16)	{ garmin->record_data.index = data; }
DECLARE_FIELD(ANY, part_index, UINT32)		{ garmin->record_data.index = data; }

// FILE msg
DECLARE_FIELD(FILE, file_type, ENUM) { }
DECLARE_FIELD(FILE, manufacturer, UINT16) { }
DECLARE_FIELD(FILE, product, UINT16) { }
DECLARE_FIELD(FILE, serial, UINT32Z) { }
DECLARE_FIELD(FILE, creation_time, UINT32) { }
DECLARE_FIELD(FILE, number, UINT16) { }
DECLARE_FIELD(FILE, other_time, UINT32) { }

// SESSION msg
DECLARE_FIELD(SESSION, start_time, UINT32)	{ garmin->dive.time = data; }
DECLARE_FIELD(SESSION, start_pos_lat, SINT32)	{ garmin->gps.SESSION.entry.lat = data; }
DECLARE_FIELD(SESSION, start_pos_long, SINT32)	{ garmin->gps.SESSION.entry.lon = data; }
DECLARE_FIELD(SESSION, nec_pos_lat, SINT32)	{ garmin->gps.SESSION.NE.lat = data; }
DECLARE_FIELD(SESSION, nec_pos_long, SINT32)	{ garmin->gps.SESSION.NE.lon = data; }
DECLARE_FIELD(SESSION, swc_pos_lat, SINT32)	{ garmin->gps.SESSION.SW.lat = data; }
DECLARE_FIELD(SESSION, swc_pos_long, SINT32)	{ garmin->gps.SESSION.SW.lon = data; }
DECLARE_FIELD(SESSION, exit_pos_lat, SINT32)	{ garmin->gps.SESSION.exit.lat = data; }
DECLARE_FIELD(SESSION, exit_pos_long, SINT32)	{ garmin->gps.SESSION.exit.lon = data; }

// LAP msg
DECLARE_FIELD(LAP, start_time, UINT32) { }
DECLARE_FIELD(LAP, start_pos_lat, SINT32)	{ garmin->gps.LAP.entry.lat = data; }
DECLARE_FIELD(LAP, start_pos_long, SINT32)	{ garmin->gps.LAP.entry.lon = data; }
DECLARE_FIELD(LAP, end_pos_lat, SINT32)		{ garmin->gps.LAP.exit.lat = data; }
DECLARE_FIELD(LAP, end_pos_long, SINT32)	{ garmin->gps.LAP.exit.lon = data; }
DECLARE_FIELD(LAP, some_pos_lat, SINT32)	{ garmin->gps.LAP.some.lat = data; }
DECLARE_FIELD(LAP, some_pos_long, SINT32)	{ garmin->gps.LAP.some.lon = data; }
DECLARE_FIELD(LAP, other_pos_lat, SINT32)	{ garmin->gps.LAP.other.lat = data; }
DECLARE_FIELD(LAP, other_pos_long, SINT32)	{ garmin->gps.LAP.other.lon = data; }

// RECORD msg
DECLARE_FIELD(RECORD, position_lat, SINT32)	{ garmin->gps.RECORD.lat = data; }
DECLARE_FIELD(RECORD, position_long, SINT32)	{ garmin->gps.RECORD.lon = data; }
DECLARE_FIELD(RECORD, altitude, UINT16) { }		// 5 *m + 500 ?
DECLARE_FIELD(RECORD, heart_rate, UINT8)		// bpm
{
	if (garmin->callback) {
		dc_sample_value_t sample = {0};
		sample.heartbeat = data;
		garmin->callback(DC_SAMPLE_HEARTBEAT, sample, garmin->userdata);
	}
}
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
	garmin->record_data.pending |= RECORD_DECO;
	garmin->record_data.ceiling = data / 1000.0;
}
DECLARE_FIELD(RECORD, next_stop_time, UINT32)		// seconds
{
	garmin->record_data.pending |= RECORD_DECO;
	garmin->record_data.stop_time = data;
}
DECLARE_FIELD(RECORD, tts, UINT32)
{
	if (garmin->callback) {
		dc_sample_value_t sample = {0};
		sample.time = data;
		garmin->callback(DC_SAMPLE_TTS, sample, garmin->userdata);
	}
}
DECLARE_FIELD(RECORD, ndl, UINT32)			// s
{
	if (garmin->callback) {
		dc_sample_value_t sample = {0};
		sample.deco.type = DC_DECO_NDL;
		sample.deco.time = data;
		garmin->callback(DC_SAMPLE_DECO, sample, garmin->userdata);
	}
}
DECLARE_FIELD(RECORD, cns_load, UINT8)
{
	if (garmin->callback) {
		dc_sample_value_t sample = {0};
		sample.cns = data / 100.0;
		garmin->callback(DC_SAMPLE_CNS, sample, garmin->userdata);
	}
}
DECLARE_FIELD(RECORD, n2_load, UINT16) { }		// percent
DECLARE_FIELD(RECORD, air_time_remaining, UINT32) { }	// seconds
DECLARE_FIELD(RECORD, pressure_sac, UINT16) { }		// 100 * bar/min/pressure
DECLARE_FIELD(RECORD, volume_sac, UINT16) { }		// 100 * l/min/pressure
DECLARE_FIELD(RECORD, rmv, UINT16) { }			// 100 * l/min
DECLARE_FIELD(RECORD, ascent_rate, SINT32) { }		// mm/s (negative is down)

// DEVICE_SETTINGS
DECLARE_FIELD(DEVICE_SETTINGS, utc_offset, UINT32) { garmin->dive.utc_offset = (SINT32) data; }	// wrong type in FIT
DECLARE_FIELD(DEVICE_SETTINGS, time_offset, UINT32) { garmin->dive.time_offset = (SINT32) data; }	// wrong type in FIT

// DEVICE_INFO
// collect the data and then use the record if it is for device_index 0
DECLARE_FIELD(DEVICE_INFO, device_index, UINT8)
{
	garmin->record_data.device_index = data;
	garmin->record_data.pending |= RECORD_DEVICE_INFO;
}
DECLARE_FIELD(DEVICE_INFO, product, UINT16)
{
	garmin->record_data.product = data;
	garmin->record_data.pending |= RECORD_DEVICE_INFO;
}
DECLARE_FIELD(DEVICE_INFO, serial_nr, UINT32Z)
{
	garmin->record_data.serial = data;
	garmin->record_data.pending |= RECORD_DEVICE_INFO;
}
DECLARE_FIELD(DEVICE_INFO, firmware, UINT16)
{
	garmin->record_data.firmware = data;
	garmin->record_data.pending |= RECORD_DEVICE_INFO;
}

// SPORT
DECLARE_FIELD(SPORT, sub_sport, ENUM) {
	garmin->dive.sub_sport = (ENUM) data;
	dc_divemode_t val;
	switch (data) {
	case 55: val = DC_DIVEMODE_GAUGE;
		break;
	case 56:
	case 57: val = DC_DIVEMODE_FREEDIVE;
		break;
	case 63: val = DC_DIVEMODE_CCR;
		break;
	default: val = DC_DIVEMODE_OC;
	}
	DC_ASSIGN_FIELD(garmin->cache, DIVEMODE, val);
}

// DIVE_GAS - uses msg index
DECLARE_FIELD(DIVE_GAS, helium, UINT8)
{
	garmin->record_data.gasmix.helium = data / 100.0;
	garmin->record_data.pending |= RECORD_GASMIX;
}
DECLARE_FIELD(DIVE_GAS, oxygen, UINT8)
{
	garmin->record_data.gasmix.oxygen = data / 100.0;
	garmin->record_data.pending |= RECORD_GASMIX;
}
DECLARE_FIELD(DIVE_GAS, status, ENUM)
{
	// 0 - disabled, 1 - enabled, 2 - backup
	garmin->record_data.gas_status = data;
}

// DIVE_SUMMARY
DECLARE_FIELD(DIVE_SUMMARY, avg_depth, UINT32) { DC_ASSIGN_FIELD(garmin->cache, AVGDEPTH, data / 1000.0); }
DECLARE_FIELD(DIVE_SUMMARY, max_depth, UINT32) { DC_ASSIGN_FIELD(garmin->cache, MAXDEPTH, data / 1000.0); }
DECLARE_FIELD(DIVE_SUMMARY, surface_interval, UINT32) { }	// sec
DECLARE_FIELD(DIVE_SUMMARY, start_cns, UINT8) { }		// percent
DECLARE_FIELD(DIVE_SUMMARY, end_cns, UINT8) { }			// percent
DECLARE_FIELD(DIVE_SUMMARY, start_n2, UINT16) { }		// percent
DECLARE_FIELD(DIVE_SUMMARY, end_n2, UINT16) { }			// percent
DECLARE_FIELD(DIVE_SUMMARY, o2_toxicity, UINT16) { }		// OTUs
DECLARE_FIELD(DIVE_SUMMARY, dive_number, UINT32) { }
DECLARE_FIELD(DIVE_SUMMARY, bottom_time, UINT32) { DC_ASSIGN_FIELD(garmin->cache, DIVETIME, data / 1000); }
DECLARE_FIELD(DIVE_SUMMARY, avg_pressure_sac, UINT16) { }	// 100 * bar/min/pressure
DECLARE_FIELD(DIVE_SUMMARY, avg_volume_sac, UINT16) { }		// 100 * L/min/pressure
DECLARE_FIELD(DIVE_SUMMARY, avg_rmv, UINT16) { }		// 100 * L/min

// DIVE_SETTINGS
DECLARE_FIELD(DIVE_SETTINGS, name, STRING) { }
DECLARE_FIELD(DIVE_SETTINGS, model, ENUM)
{
	garmin->record_data.model = data;
	garmin->record_data.pending |= RECORD_DECO_MODEL;
}
DECLARE_FIELD(DIVE_SETTINGS, gf_low, UINT8)
{
	garmin->record_data.gf_low = data;
	garmin->record_data.pending |= RECORD_DECO_MODEL;
}
DECLARE_FIELD(DIVE_SETTINGS, gf_high, UINT8)
{
	garmin->record_data.gf_high = data;
	garmin->record_data.pending |= RECORD_DECO_MODEL;
}
DECLARE_FIELD(DIVE_SETTINGS, water_type, ENUM)
{
	garmin->cache.SALINITY.type = data ? DC_WATER_SALT : DC_WATER_FRESH;
	garmin->cache.initialized |= 1 << DC_FIELD_SALINITY;
}
DECLARE_FIELD(DIVE_SETTINGS, water_density, FLOAT)
{
	union { unsigned int binary; float actual; } val;
	val.binary = data;
	garmin->cache.SALINITY.density = val.actual;
	garmin->cache.initialized |= 1 << DC_FIELD_SALINITY;
}
DECLARE_FIELD(DIVE_SETTINGS, po2_warn, UINT8) { }
DECLARE_FIELD(DIVE_SETTINGS, po2_critical, UINT8) { }
DECLARE_FIELD(DIVE_SETTINGS, po2_deco, UINT8) { }
DECLARE_FIELD(DIVE_SETTINGS, safety_stop_enabled, ENUM) { }
DECLARE_FIELD(DIVE_SETTINGS, bottom_depth, FLOAT) { }
DECLARE_FIELD(DIVE_SETTINGS, bottom_time, UINT32) { }
DECLARE_FIELD(DIVE_SETTINGS, apnea_countdown_enabled, ENUM) { }
DECLARE_FIELD(DIVE_SETTINGS, apnea_countdown_time, UINT32) { }
DECLARE_FIELD(DIVE_SETTINGS, backlight_mode, ENUM) { }
DECLARE_FIELD(DIVE_SETTINGS, backlight_brightness, UINT8) { }
DECLARE_FIELD(DIVE_SETTINGS, backlight_timeout, UINT8) { }
DECLARE_FIELD(DIVE_SETTINGS, repeat_dive_interval, UINT16) { }
DECLARE_FIELD(DIVE_SETTINGS, safety_stop_time, UINT16) { }
DECLARE_FIELD(DIVE_SETTINGS, heart_rate_source_type, ENUM) { }
DECLARE_FIELD(DIVE_SETTINGS, heart_rate_device_type, UINT8) { }
DECLARE_FIELD(DIVE_SETTINGS, setpoint_low_cbar, UINT8)
{
	garmin->dive.setpoint_low_cbar = data;

	// The initial setpoint at the start of the dive is the low setpoint
	garmin->record_data.setpoint_actual_cbar = garmin->dive.setpoint_low_cbar;
	garmin->record_data.pending |= RECORD_SETPOINT_CHANGE;
}
DECLARE_FIELD(DIVE_SETTINGS, setpoint_low_switch_depth_mm, UINT32)
{
	garmin->dive.setpoint_low_switch_depth_mm = data;
}
DECLARE_FIELD(DIVE_SETTINGS, setpoint_high_cbar, UINT8)
{
	garmin->dive.setpoint_high_cbar = data;
}
DECLARE_FIELD(DIVE_SETTINGS, setpoint_high_switch_depth_mm, UINT32)
{
	garmin->dive.setpoint_high_switch_depth_mm = data;
}

// SENSOR_PROFILE record for each ANT/BLE sensor.
// We only care about sensor type 28 - Garmin tank pod.
DECLARE_FIELD(SENSOR_PROFILE, ant_channel_id, UINT32Z)
{
	current_sensor(garmin)->sensor_id = data;
}
DECLARE_FIELD(SENSOR_PROFILE, name, STRING) { } // We don't pass in string types correctly
DECLARE_FIELD(SENSOR_PROFILE, enabled, ENUM)
{
	current_sensor(garmin)->sensor_enabled = data;
}
DECLARE_FIELD(SENSOR_PROFILE, sensor_type, UINT8)
{
	// 28 is tank pod
	// start filling in next sensor after this record
	if (data == 28)
		garmin->record_data.pending |= RECORD_SENSOR_PROFILE;
}
DECLARE_FIELD(SENSOR_PROFILE, pressure_units, ENUM)
{
	//  0 is PSI, 1 is KPA (unused), 2 is Bar
	current_sensor(garmin)->sensor_units = data;
}
DECLARE_FIELD(SENSOR_PROFILE, rated_pressure, UINT16)
{
	current_sensor(garmin)->sensor_rated_pressure = data;
}
DECLARE_FIELD(SENSOR_PROFILE, reserve_pressure, UINT16)
{
	current_sensor(garmin)->sensor_reserve_pressure = data;
}
DECLARE_FIELD(SENSOR_PROFILE, volume, UINT16)
{
	current_sensor(garmin)->sensor_volume = data;
}
DECLARE_FIELD(SENSOR_PROFILE, used_for_gas_rate, ENUM)
{
	current_sensor(garmin)->sensor_used_for_gas_rate = data;
}

DECLARE_FIELD(TANK_UPDATE, sensor, UINT32Z)
{
	garmin->record_data.sensor = data;
}

DECLARE_FIELD(TANK_UPDATE, pressure, UINT16)
{
	garmin->record_data.pressure = data;
	garmin->record_data.pending |= RECORD_TANK_UPDATE;
}

DECLARE_FIELD(TANK_SUMMARY, sensor, UINT32Z) { }	// sensor ID
DECLARE_FIELD(TANK_SUMMARY, start_pressure, UINT16) { }	// Bar * 100
DECLARE_FIELD(TANK_SUMMARY, end_pressure, UINT16) { }	// Bar * 100
DECLARE_FIELD(TANK_SUMMARY, volume_used, UINT32) { }	// L * 100

// EVENT
DECLARE_FIELD(EVENT, event, ENUM)
{
	garmin->record_data.event_nr = data;
	garmin->record_data.pending |= RECORD_EVENT;
}
DECLARE_FIELD(EVENT, type, ENUM)
{
	garmin->record_data.event_type = data;
	garmin->record_data.pending |= RECORD_EVENT;
}
DECLARE_FIELD(EVENT, data, UINT32)
{
	garmin->record_data.event_data = data;
}
DECLARE_FIELD(EVENT, event_group, UINT8)
{
	garmin->record_data.event_group = data;
}
DECLARE_FIELD(EVENT, unknown, UINT32)
{
	garmin->record_data.event_unknown = data;
}
DECLARE_FIELD(EVENT, tank_pressure_reserve, UINT32Z) { }	// sensor ID
DECLARE_FIELD(EVENT, tank_pressure_critical, UINT32Z) { }	// sensor ID
DECLARE_FIELD(EVENT, tank_pressure_lost, UINT32Z) { }		// sensor ID

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

DECLARE_MESG(SPORT) = {
	.maxfield = 2,
	.field = {
		SET_FIELD(SPORT, 1, sub_sport, ENUM),	// 53 - 57 and 63 are dive activities
	}
};

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
	.maxfield = 128,
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
		SET_FIELD(RECORD, 123, air_time_remaining, UINT32),	// seconds
		SET_FIELD(RECORD, 124, pressure_sac, UINT16),	// 100 * bar/min/pressure
		SET_FIELD(RECORD, 125, volume_sac, UINT16),	// 100 * l/min/pressure
		SET_FIELD(RECORD, 126, rmv, UINT16),		// 100 * l/min
		SET_FIELD(RECORD, 127, ascent_rate, SINT32),	// mm/s (negative is down)
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
	.maxfield = 15,
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
		SET_FIELD(DIVE_SUMMARY, 12, avg_pressure_sac, UINT16),	// 100 * bar/min/pressure
		SET_FIELD(DIVE_SUMMARY, 13, avg_volume_sac, UINT16),	// 100 * L/min/pressure
		SET_FIELD(DIVE_SUMMARY, 14, avg_rmv, UINT16),		// 100 * L/min
	}
};

DECLARE_MESG(EVENT) = {
	.maxfield = 74,
	.field = {
		SET_FIELD(EVENT, 0, event, ENUM),
		SET_FIELD(EVENT, 1, type, ENUM),
		SET_FIELD(EVENT, 3, data, UINT32),
		SET_FIELD(EVENT, 4, event_group, UINT8),
		SET_FIELD(EVENT, 15, unknown, UINT32),
		SET_FIELD(EVENT, 71, tank_pressure_reserve, UINT32Z),	// sensor ID
		SET_FIELD(EVENT, 72, tank_pressure_critical, UINT32Z),	// sensor ID
		SET_FIELD(EVENT, 73, tank_pressure_lost, UINT32Z),	// sensor ID
	}
};

DECLARE_MESG(DEVICE_INFO) = {
	.maxfield = 6,
	.field = {
		SET_FIELD(DEVICE_INFO, 0, device_index, UINT8),
		SET_FIELD(DEVICE_INFO, 3, serial_nr, UINT32Z),
		SET_FIELD(DEVICE_INFO, 4, product, UINT16),
		SET_FIELD(DEVICE_INFO, 5, firmware, UINT16),
	}
};

DECLARE_MESG(ACTIVITY) = { };
DECLARE_MESG(FILE_CREATOR) = { };

DECLARE_MESG(DIVE_SETTINGS) = {
	.maxfield = 28,
	.field = {
		SET_FIELD(DIVE_SETTINGS, 0, name, STRING),		// Unused except in dive plans
		SET_FIELD(DIVE_SETTINGS, 1, model, ENUM),		// model - Always 0 for Buhlmann ZHL-16C
		SET_FIELD(DIVE_SETTINGS, 2, gf_low, UINT8),		// 0 to 100
		SET_FIELD(DIVE_SETTINGS, 3, gf_high, UINT8),		// 0 to 100
		SET_FIELD(DIVE_SETTINGS, 4, water_type, ENUM),		// One of fresh (0), salt (1), or custom (3). 2 is en13319 which is unused.
		SET_FIELD(DIVE_SETTINGS, 5, water_density, FLOAT),	// If water_type is custom, this will be the density. Fresh is usually 1000, salt is usually 1025
		SET_FIELD(DIVE_SETTINGS, 6, po2_warn, UINT8),		// PO2 * 100, so typically 140 to 160. When the PO2 starts blinking yellow
		SET_FIELD(DIVE_SETTINGS, 7, po2_critical, UINT8),	// See above; value when PO2 blinks red and you get a popup
		SET_FIELD(DIVE_SETTINGS, 8, po2_deco, UINT8),		// See above; PO2 limited used for choosing which gas to suggest
		SET_FIELD(DIVE_SETTINGS, 9, safety_stop_enabled, ENUM),	// Used in conjunction with safety_stop_time below
		SET_FIELD(DIVE_SETTINGS, 10, bottom_depth, FLOAT),	// Unused except in dive plans
		SET_FIELD(DIVE_SETTINGS, 11, bottom_time, UINT32),	// Unused except in dive plans
		SET_FIELD(DIVE_SETTINGS, 12, apnea_countdown_enabled, ENUM), // This and apnea_countdown_time are the "Apnea Surface Alert" setting
		SET_FIELD(DIVE_SETTINGS, 13, apnea_countdown_time, UINT32), //
		SET_FIELD(DIVE_SETTINGS, 14, backlight_mode, ENUM),	// 0 is "At Depth" and 1 is "Always On"
		SET_FIELD(DIVE_SETTINGS, 15, backlight_brightness, UINT8), // 0 to 100
		SET_FIELD(DIVE_SETTINGS, 16, backlight_timeout, UINT8),	// seconds; 0 is no timeout
		SET_FIELD(DIVE_SETTINGS, 17, repeat_dive_interval, UINT16), // seconds between surfacing and when the watch stops and saves your dive. Must be at least 20.
		SET_FIELD(DIVE_SETTINGS, 18, safety_stop_time, UINT16),	// seconds; 180 or 300 are acceptable values
		SET_FIELD(DIVE_SETTINGS, 19, heart_rate_source_type, ENUM), // For now all you need to know is source_type_local means WHR and source_type_antplus means strap data or off. (We're reusing existing infrastructure here which is why this is complex.)
		SET_FIELD(DIVE_SETTINGS, 20, heart_rate_device_type, UINT8), // device type depending on heart_rate_source_type (ignorable for now)
		SET_FIELD(DIVE_SETTINGS, 23, setpoint_low_cbar, UINT8), // CCR low setpoint [centibar]
		SET_FIELD(DIVE_SETTINGS, 24, setpoint_low_switch_depth_mm, UINT32), // CCR low setpoint switch depth [mm]
		SET_FIELD(DIVE_SETTINGS, 26, setpoint_high_cbar, UINT8), // CCR high setpoint [centibar]
		SET_FIELD(DIVE_SETTINGS, 27, setpoint_high_switch_depth_mm, UINT32), // CCR high setpoint switch depth [mm]
	}
};
DECLARE_MESG(DIVE_ALARM) = { };

DECLARE_MESG(SENSOR_PROFILE) = {
	.maxfield = 79,
	.field = {
		SET_FIELD(SENSOR_PROFILE, 0, ant_channel_id, UINT32Z),	// derived from the number engraved on the side
		SET_FIELD(SENSOR_PROFILE, 2, name, STRING),
		SET_FIELD(SENSOR_PROFILE, 3, enabled, ENUM),
		SET_FIELD(SENSOR_PROFILE, 52, sensor_type, UINT8),	// 28 is tank pod
		SET_FIELD(SENSOR_PROFILE, 74, pressure_units, ENUM),	//  0 is PSI, 1 is KPA (unused), 2 is Bar
		SET_FIELD(SENSOR_PROFILE, 75, rated_pressure, UINT16),
		SET_FIELD(SENSOR_PROFILE, 76, reserve_pressure, UINT16),
		SET_FIELD(SENSOR_PROFILE, 77, volume, UINT16),		// CuFt * 10 (PSI) or L * 10 (Bar)
		SET_FIELD(SENSOR_PROFILE, 78, used_for_gas_rate, ENUM),	// was used for SAC calculations?
	}
};

DECLARE_MESG(TANK_UPDATE) = {
	.maxfield = 2,
	.field = {
		SET_FIELD(TANK_UPDATE, 0, sensor, UINT32Z),		// sensor ID
		SET_FIELD(TANK_UPDATE, 1, pressure, UINT16),		// pressure in Bar * 100
	}
};

DECLARE_MESG(TANK_SUMMARY) = {
	.maxfield = 4,
	.field = {
		SET_FIELD(TANK_SUMMARY, 0, sensor, UINT32Z),		// sensor ID
		SET_FIELD(TANK_SUMMARY, 1, start_pressure, UINT16),	// Bar * 100
		SET_FIELD(TANK_SUMMARY, 2, end_pressure, UINT16),	// Bar * 100
		SET_FIELD(TANK_SUMMARY, 3, volume_used, UINT32),	// L * 100
	}
};

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

	SET_MESG(147, SENSOR_PROFILE),

	SET_MESG(216, WTF_216),
	SET_MESG(233, WTF_233),
	SET_MESG(258, DIVE_SETTINGS),
	SET_MESG(259, DIVE_GAS),
	SET_MESG(262, DIVE_ALARM),
	SET_MESG(268, DIVE_SUMMARY),
	SET_MESG(319, TANK_UPDATE),
	SET_MESG(323, TANK_SUMMARY),
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

static int all_data_inval(const unsigned char *data, int base_type, int len)
{
	int base_size = base_type_info[base_type].type_size;
	unsigned long long invalid = base_type_info[base_type].type_inval;

	while (len > 0) {
		unsigned long long val = 0;
		memcpy(&val, data, base_size);
		if (val != invalid)
			return 0;
		data += base_size;
		len -= base_size;
	}
	return 1;
}

static void unknown_field(struct garmin_parser_t *garmin, const unsigned char *data,
	const char *msg_name, unsigned int field_nr,
	int base_type, int len)
{
	char buffer[80];
	const char *str = (const char *)data;

	/* Skip empty strings */
	if (base_type == 7 && !*str)
		return;

	/* Turn non-string data into hex values */
	if (base_type != 7) {
		int pos = 0;
		int base_size = base_type_info[base_type].type_size;
		const char *sep = "";

		/* Skip empty data */
		if (all_data_inval(data, base_type, len))
			return;

		str = buffer;
		while (len > 0) {
			long long val;
			/* Space + hex + NUL */
			int need = 2+base_size*2;

			/* The "-4" is because we reserve that " ..\0" at the end */
			if (pos + need >= sizeof(buffer)-4) {
				strcpy(buffer+pos, " ..");
				break;
			}

			val = 0;
			memcpy(&val, data, base_size);

			pos += sprintf(buffer+pos, "%s%0*llx", sep, base_size*2, val);
			sep = " ";

			data += base_size;
			len -= base_size;
		}
	}

	DEBUG(garmin->base.context, "%s/%d %s '%s'", msg_name, field_nr, base_type_info[base_type].type_name, str);
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
		base_size = base_type_info[base_type].type_size;
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
			unknown_field(garmin, data, msg_name, field_nr, base_type, len);
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

	// data[1] tells us if this is big or little endian
	garmin->is_big_endian = data[1] != 0;
	msg = array_uint_endian(data + 2, 2, garmin->is_big_endian);
	desc->msg_desc = lookup_msg_desc(msg, type, &desc->msg_name);
	fields = data[4];
	DEBUG(garmin->base.context, "Define local type %d: %02x %s %04x %02x %s",
		type, data[0], data[1] ? "big-endian" : "little-endian", msg, fields, desc->msg_name);
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
	memset(&garmin->record_data, 0, sizeof(garmin->record_data));
	memset(garmin->type_desc, 0, sizeof(garmin->type_desc));

	// The data starts with our filename fingerprint. Skip it.
	if (len < FIT_NAME_SIZE)
		return DC_STATUS_IO;

	DEBUG(garmin->base.context, "file %s", data);

	data += FIT_NAME_SIZE;
	len -= FIT_NAME_SIZE;

	// The FIT header
	if (len < 12)
		return DC_STATUS_IO;

	hdrsize = data[0];
	protocol = data[1];
	profile = array_uint16_le(data+2);  // these two fields are always little endian
	datasize = array_uint32_le(data+4);
	if (memcmp(data+8, ".FIT", 4)) {
		DEBUG(garmin->base.context, " missing .FIT marker");
		return DC_STATUS_IO;
	}
	if (hdrsize < 12 || datasize > len || datasize + hdrsize + 2 > len) {
		DEBUG(garmin->base.context, " inconsistent size information hdrsize %d datasize %d len %d", hdrsize, datasize, len);
		return DC_STATUS_IO;
	}
	garmin->dive.protocol = protocol;
	garmin->dive.profile = profile;

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

		// Flush pending data on record boundaries
		if (garmin->record_data.pending)
			flush_pending_record(garmin);
	}

	return DC_STATUS_SUCCESS;
}

/* Don't use floating point printing, because of "," vs "." confusion */
static void add_gps_string(garmin_parser_t *garmin, const char *desc, struct pos *pos)
{
	int lat = pos->lat, lon = pos->lon;

	if (lat && lon) {
		int latsign = 0, lonsign = 0;
		int latfrac, lonfrac;
		long long tmp;

		if (lat < 0) {
			lat = -lat;
			latsign = 1;
		}
		if (lon < 0) {
			lon = -lon;
			lonsign = 1;
		}

		tmp = 360 * (long long) lat;
		lat = tmp >> 32;
		tmp &= 0xffffffff;
		tmp *= 1000000;
		latfrac = tmp >> 32;

		tmp = 360 * (long long) lon;
		lon = tmp >> 32;
		tmp &= 0xffffffff;
		tmp *= 1000000;
		lonfrac = tmp >> 32;

		dc_field_add_string_fmt(&garmin->cache, desc, "%s%d.%06d, %s%d.%06d",
			latsign ? "-" : "", lat, latfrac,
			lonsign ? "-" : "", lon, lonfrac);
	}
}

int
garmin_parser_is_dive (dc_parser_t *abstract, const unsigned char *data, unsigned int size, dc_event_devinfo_t *devinfo_p)
{
	// set up the parser and extract data
	dc_parser_set_data(abstract, data, size);
	garmin_parser_t *garmin = (garmin_parser_t *) abstract;

	if (devinfo_p) {
		devinfo_p->firmware = garmin->dive.firmware;
		devinfo_p->serial = garmin->dive.serial;
		devinfo_p->model = garmin->dive.product;
	}
	switch (garmin->dive.sub_sport) {
	case 53:	// Single-gas
	case 54:	// Multi-gas
	case 55:	// Gauge
	case 56:	// Apnea
	case 57:	// Apnea Hunt
	case 63:	// CCR
		return 1;
	default:
		// Even if we don't recognize the sub_sport,
		// let's assume it's a dive if we've seen
		// average depth in the  DIVE_SUMMARY record.
		if (garmin->cache.AVGDEPTH)
			return 1;
		return 0;
	}
}

static void add_sensor_string(garmin_parser_t *garmin, const char *desc, const struct garmin_sensor *sensor)
{
	dc_field_add_string_fmt(&garmin->cache, desc, "%x", sensor->sensor_id);
}

static dc_status_t
garmin_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	garmin_parser_t *garmin = (garmin_parser_t *) abstract;

	/* Walk the data once without a callback to set up the core fields */
	garmin->callback = NULL;
	garmin->userdata = NULL;
	memset(&garmin->gps, 0, sizeof(garmin->gps));
	memset(&garmin->dive, 0, sizeof(garmin->dive));
	memset(&garmin->cache, 0, sizeof(garmin->cache));

	traverse_data(garmin);

	// Device information
	dc_field_add_string_fmt(&garmin->cache, "Serial", "%u", garmin->dive.serial);
	dc_field_add_string_fmt(&garmin->cache, "Firmware", "%u.%02u",
		garmin->dive.firmware / 100, garmin->dive.firmware % 100);

	// These seem to be the "real" GPS dive coordinates
	add_gps_string(garmin, "GPS1", &garmin->gps.SESSION.entry);
	add_gps_string(garmin, "GPS2", &garmin->gps.SESSION.exit);

	add_gps_string(garmin, "Session NE corner GPS", &garmin->gps.SESSION.NE);
	add_gps_string(garmin, "Session SW corner GPS", &garmin->gps.SESSION.SW);

	add_gps_string(garmin, "Lap entry GPS", &garmin->gps.LAP.entry);
	add_gps_string(garmin, "Lap exit GPS", &garmin->gps.LAP.exit);
	add_gps_string(garmin, "Lap some GPS", &garmin->gps.LAP.some);
	add_gps_string(garmin, "Lap other GPS", &garmin->gps.LAP.other);

	add_gps_string(garmin, "Record GPS", &garmin->gps.RECORD);

	// We have no idea about gas mixes vs tanks
	for (int i = 0; i < garmin->dive.nr_sensor; i++) {
		// DC_ASSIGN_IDX(garmin->cache, tankinfo, i, ..);
		// DC_ASSIGN_IDX(garmin->cache, tanksize, i, ..);
		// DC_ASSIGN_IDX(garmin->cache, tankworkingpressure, i, ..);
	}

	// Hate hate hate gasmix vs tank counts.
	//
	// There's no way to match them up unless they are an identity
	// mapping, so having two different ones doesn't actually work.
	if (garmin->dive.nr_sensor > garmin->cache.GASMIX_COUNT)
		DC_ASSIGN_FIELD(garmin->cache, GASMIX_COUNT, garmin->dive.nr_sensor);

	for (int i = 0; i < garmin->dive.nr_sensor; i++) {
		static const char *name[] = { "Sensor 1", "Sensor 2", "Sensor 3", "Sensor 4", "Sensor 5" };
		add_sensor_string(garmin, name[i], garmin->dive.sensor+i);
	}

	dc_field_add_string_fmt(&garmin->cache, "Setpoint low auto switch depth [m]", "%u.%01u",
		garmin->dive.setpoint_low_switch_depth_mm / 1000, (garmin->dive.setpoint_low_switch_depth_mm % 1000) / 100);
	dc_field_add_string_fmt(&garmin->cache, "Setpoint high auto switch depth [m]", "%u.%01u",
		garmin->dive.setpoint_high_switch_depth_mm / 1000, (garmin->dive.setpoint_high_switch_depth_mm % 1000) / 100);


	return DC_STATUS_SUCCESS;
}


static dc_status_t
garmin_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	garmin_parser_t *garmin = (garmin_parser_t *) abstract;
	dc_ticks_t time = 631065600 + (dc_ticks_t) garmin->dive.time;

	// Show local time (time_offset)
	dc_datetime_gmtime(datetime, time + garmin->dive.time_offset);
	datetime->timezone = DC_TIMEZONE_NONE;

	return DC_STATUS_SUCCESS;
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

static dc_status_t
garmin_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	garmin_parser_t *garmin = (garmin_parser_t *) abstract;

	return dc_field_get(&garmin->cache, type, flags, value);
}

static dc_status_t
garmin_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	garmin_parser_t *garmin = (garmin_parser_t *) abstract;

	garmin->callback = callback;
	garmin->userdata = userdata;
	return traverse_data(garmin);
}
