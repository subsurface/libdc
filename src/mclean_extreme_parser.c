/*
 * libdivecomputer
 *
 * Copyright (C) 2018 Jef Driesen
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

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "mclean_extreme.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_device_isinstance((parser), &mclean_extreme_parser_vtable)

#define SZ_CFG				0x002d				// size of the common dive/computer header
#define SZ_COMPUTER			0x006a				// size of the computer state dump
#define SZ_DIVE				0x005e				// size of the dive state dump
#define SZ_SAMPLE			0x0004				// size of the sample state dump

// private dive parsing functions //////////////////////////////////////////////////////////////////////////////////////////

static uint16_t uint16(const unsigned char* buffer, int addr) { return (buffer[0 + addr] << 0) | (buffer[1 + addr] << 8); }
static uint32_t uint32(const unsigned char* buffer, int addr) { return (buffer[0 + addr] << 0) | (buffer[1 + addr] << 8) | (buffer[2 + addr] << 16) | (buffer[3 + addr] << 24); }

static uint8_t dive_format(const unsigned char* dive) { return dive[0x0000]; }
static uint8_t dive_gas_pO2(const unsigned char* dive, int value) { return dive[0x0001 + value * 2]; }
static uint8_t dive_gas_pHe(const unsigned char* dive, int value) { return dive[0x0001 + 1 + value * 2]; }
// static bool dive_gas_enabled(const unsigned char* dive, int value) { return (dive[0x0011] & (1 << value)) != 0; }
static uint8_t dive_setpoint(const unsigned char* dive, int value) { return dive[0x0013 + value]; }
// static bool dive_setpoint_enabled(const unsigned char* dive, int value) { return (dive[dive, 0x0016] & (1 << value)) != 0; }
// static bool dive_metric(const unsigned char* dive) { return dive[0x0018] != 0; }
// static uint16_t dive_name(const unsigned char* dive) { return uint16(dive, 0x0019); }
// static uint16_t dive_Vasc(const unsigned char* dive) { return uint16(dive, 0x001c); }
static uint16_t dive_Psurf(const unsigned char* dive) { return uint16(dive, 0x001e); }
// static uint8_t dive_gfs_index(const unsigned char* dive) { return dive[0x0020]; }
// static uint8_t dive_gflo(const unsigned char* dive) { return dive[0x0021]; }
// static uint8_t dive_gfhi(const unsigned char* dive) { return dive[0x0022]; }
static uint8_t dive_density_index(const unsigned char* dive) { return dive[0x0023]; }
// static uint16_t dive_ppN2_limit(const unsigned char* dive) { return uint16(dive, 0x0024); }
// static uint16_t dive_ppO2_limit(const unsigned char* dive) { return uint16(dive, 0x0026); }
// static uint16_t dive_ppO2_bottomlimit(const unsigned char* dive) { return uint16(dive, 0x0028); }
// static uint16_t dive_density_limit(const unsigned char* dive) { return uint16(dive, 0x002a); }
static uint8_t dive_operatingmode(const unsigned char* dive) { return dive[0x002c]; }

static uint32_t dive_logstart(const unsigned char* dive) { return uint32(dive, SZ_CFG + 0x0000); }
static uint32_t dive_divestart(const unsigned char* dive) { return uint32(dive, SZ_CFG + 0x0004); }
static uint32_t dive_diveend(const unsigned char* dive) { return uint32(dive, SZ_CFG + 0x0008); }
static uint32_t dive_logend(const unsigned char* dive) { return uint32(dive, SZ_CFG + 0x000c); }
static uint8_t dive_temp_min(const unsigned char* dive) { return dive[SZ_CFG + 0x0010]; }
static uint8_t dive_temp_max(const unsigned char* dive) { return dive[SZ_CFG + 0x0011]; }
// static uint16_t dive_pO2_min(const unsigned char* dive) { return uint16(dive, SZ_CFG + 0x0012); }
// static uint16_t dive_pO2_max(const unsigned char* dive) { return uint16(dive, SZ_CFG + 0x0014); }
static uint16_t dive_Pmax(const unsigned char* dive) { return uint16(dive, SZ_CFG + 0x0016); }
static uint16_t dive_Pav(const unsigned char* dive) { return uint16(dive, SZ_CFG + 0x0018); }
// static uint32_t ISS(const unsigned char* dive) { return uint16(dive, SZ_CFG + 0x001a); }
// static uint16_t CNS_start(const unsigned char* dive) { return uint16(dive, SZ_CFG + 0x001e); }
// static uint16_t CNS_max(const unsigned char* dive) { return uint16(dive, SZ_CFG + 0x0020); }
// static uint16_t OTU(const unsigned char* dive) { return uint16(dive, SZ_CFG + 0x0022); }
// static uint16_t tndl(const unsigned char* dive) { return uint16(dive, SZ_CFG + 0x0024); }
// static uint32_t tdeco(const unsigned char* dive) { return uint32(dive, SZ_CFG + 0x0026); }
// static uint8_t ndeco(const unsigned char* dive) { return dive[SZ_CFG + 0x002a]; }
// static uint32_t tdesat(const unsigned char* dive) { return uint32(dive, SZ_CFG + 0x002b); }
static uint16_t dive_samples_cnt(const unsigned char* dive) { return uint16(dive, 0x005c); }

// private sample parsing functions //////////////////////////////////////////////////////////////////////////////////////////

static uint16_t sample_depth(const unsigned char* dive, int n) { return uint16(dive, SZ_DIVE + n * SZ_SAMPLE + 0); }
static uint8_t sample_temperature(const unsigned char* dive, int n) { return dive[SZ_DIVE + n * SZ_SAMPLE + 2]; }
static bool sample_ccr(const unsigned char* dive, int n) { return dive[SZ_DIVE + n * SZ_SAMPLE + 3] & 0b10000000; }
static uint8_t sample_sp_index(const unsigned char* dive, int n) { return (dive[SZ_DIVE + n * SZ_SAMPLE + 3]  & 0b01100000) >> 5; }
static uint8_t sample_gas_index(const unsigned char* dive, int n) { return (dive[SZ_DIVE + n * SZ_SAMPLE + 3] & 0b00011100) >> 2; }

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct mclean_extreme_parser_t mclean_extreme_parser_t;

struct mclean_extreme_parser_t {
	dc_parser_t base;
};

static dc_status_t mclean_extreme_parser_set_data(dc_parser_t* abstract, const unsigned char* data, unsigned int size);
static dc_status_t mclean_extreme_parser_get_datetime(dc_parser_t* abstract, dc_datetime_t* datetime);
static dc_status_t mclean_extreme_parser_get_field(dc_parser_t* abstract, dc_field_type_t type, unsigned int flags, void* value);
static dc_status_t mclean_extreme_parser_samples_foreach(dc_parser_t* abstract, dc_sample_callback_t callback, void* userdata);

static const dc_parser_vtable_t mclean_extreme_parser_vtable = {
	sizeof(mclean_extreme_parser_t),
	DC_FAMILY_MCLEAN_EXTREME,
	mclean_extreme_parser_set_data, /* set_data */
	mclean_extreme_parser_get_datetime, /* datetime */
	mclean_extreme_parser_get_field, /* fields */
	mclean_extreme_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

dc_status_t
mclean_extreme_parser_create(dc_parser_t** out, dc_context_t* context)
{
	mclean_extreme_parser_t* parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (mclean_extreme_parser_t*)dc_parser_allocate(context, &mclean_extreme_parser_vtable);
	if (parser == NULL) {
		ERROR(context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	*out = (dc_parser_t*)parser;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mclean_extreme_parser_set_data(dc_parser_t* abstract, const unsigned char* data, unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;

	if (dive_format(data) != 0) {
		status = DC_STATUS_DATAFORMAT;
		ERROR(abstract->context, "Unsupported dive format.");
	}

	const int samples_cnt = dive_samples_cnt(data);

	if (size != SZ_DIVE + samples_cnt * SZ_SAMPLE) {
		ERROR(abstract->context, "Corrupt dive in memory.");
		return DC_STATUS_DATAFORMAT;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mclean_extreme_parser_get_datetime(dc_parser_t* abstract, dc_datetime_t* datetime)
{
	if (datetime) {
		const unsigned char* dive = abstract->data;
		dc_ticks_t dc_ticks = 946684800 + dive_logstart(dive);	// raw times are offsets for 1/1/2000

		dc_datetime_gmtime(datetime, dc_ticks);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mclean_extreme_parser_get_field(dc_parser_t* abstract, dc_field_type_t type, unsigned int flags, void* value)
{
	static const double densities[] = { 1.000, 1.020, 1.030 };
	static const dc_divemode_t divemodes[] = { DC_DIVEMODE_OC, DC_DIVEMODE_OC, DC_DIVEMODE_CCR, DC_DIVEMODE_GAUGE };

	const unsigned char* dive = abstract->data;
	dc_gasmix_t* gasmix = (dc_gasmix_t*)value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int*)value) = dive_logend(dive) - dive_logstart(dive);
			break;

		case DC_FIELD_MAXDEPTH:
			*((double*)value) = 0.01 * (dive_Pmax(dive) - dive_Psurf(dive)) / densities[dive_density_index(dive)];
			break;

		case DC_FIELD_AVGDEPTH:
			*((double*)value) = 0.01 * (dive_Pav(dive) - dive_Psurf(dive)) / densities[dive_density_index(dive)];
			break;

		case DC_FIELD_SALINITY:
			switch (dive_density_index(dive)) {
			case 0: ((dc_salinity_t*)value)->type = DC_WATER_FRESH; ((dc_salinity_t*)value)->density = densities[dive_density_index(dive)]; break;
			case 1: ((dc_salinity_t*)value)->type = DC_WATER_SALT; ((dc_salinity_t*)value)->density = densities[dive_density_index(dive)]; break;
			case 2: ((dc_salinity_t*)value)->type = DC_WATER_SALT; ((dc_salinity_t*)value)->density = densities[dive_density_index(dive)]; break;
			default: /* that's an error */ break;
			}
			break;

		case DC_FIELD_ATMOSPHERIC:
			*((double*)value) = dive_Psurf(dive) / 1000.0;
			break;

		// case DC_FIELD_TEMPERATURE_SURFACE:

		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double*)value) = dive_temp_min(dive);
			break;

		case DC_FIELD_TEMPERATURE_MAXIMUM:
			*((double*)value) = dive_temp_max(dive);
			break;

		case DC_FIELD_DIVEMODE:
			*((dc_divemode_t*)value) = divemodes[dive_operatingmode(dive)];
			break;

		//case DC_FIELD_TANK:
		//case DC_FIELD_TANK_COUNT:

		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int*)value) = 8;
			break;

		case DC_FIELD_GASMIX:
			gasmix->helium = 0.01 * dive_gas_pHe(dive, flags);
			gasmix->oxygen = 0.01 * dive_gas_pO2(dive, flags);
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;

		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mclean_extreme_parser_samples_foreach(dc_parser_t* abstract, dc_sample_callback_t callback, void* userdata)
{
	const unsigned char* dive = abstract->data;
	const unsigned int size = abstract->size;

	const unsigned int interval = 20;							// this should be dive[log end] - dive[log start]?
	const int samples_cnt = dive_samples_cnt(dive);					// number of samples to follow

	if (callback) {
		unsigned int time = 0;

		for (int i = 0; i < samples_cnt; ++i) {
			dc_sample_value_t sample = { 0 };

			sample.time = time;
			callback(DC_SAMPLE_TIME, sample, userdata);

			sample.depth = sample_depth(dive, i) * 0.1;
			callback(DC_SAMPLE_DEPTH, sample, userdata);

			sample.temperature = sample_temperature(dive, i);
			callback(DC_SAMPLE_TEMPERATURE, sample, userdata);

			sample.gasmix = sample_gas_index(dive, i);
			callback(DC_SAMPLE_GASMIX, sample, userdata);

			if (sample_ccr(dive, i)) {
				const uint8_t sp_index = sample_sp_index(dive, i);

				sample.setpoint = 100.0 * dive_setpoint(dive, sp_index);
				if (callback) callback(DC_SAMPLE_SETPOINT, sample, userdata);
			}

			time += interval;
		}
	}

	return DC_STATUS_SUCCESS;
}
