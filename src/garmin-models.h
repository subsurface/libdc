// AI-generated (Claude)
/*
 * libdivecomputer
 *
 * Copyright (C) 2026 Michael Keller
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

#ifndef GARMIN_MODELS_H
#define GARMIN_MODELS_H

#include <stdbool.h>

typedef struct {
	const char *name;
	int id;
	bool mtp_capable;
} garmin_model_t;

/* FIT product IDs. See the Garmin Connect IQ device reference. */
#define GARMIN_MODEL_LIST(MODEL) \
	MODEL("Descent™ G1 / G1 Solar", 4005, true) \
	MODEL("Descent™ G2", 4588, true) \
	MODEL("Descent™ Mk1", 2859, false) \
	MODEL("Descent™ Mk1 APAC", 2991, false) \
	MODEL("Descent™ Mk2(i)", 3258, true) \
	MODEL("Descent™ Mk2(i) APAC", 3702, true) \
	MODEL("Descent™ Mk2 S", 3542, true) \
	MODEL("Descent™ Mk2 S APAC", 3930, true) \
	MODEL("Descent™ Mk3(i) 43mm", 4222, true) \
	MODEL("Descent™ Mk3(i) 51mm", 4223, true) \
	MODEL("Descent™ X50i", 4518, true) \
	MODEL("fēnix® 8 43mm", 4534, true) \
	MODEL("fēnix® 8 47mm / 51mm / tactix® 8 47mm / 51mm / quatix® 8 47mm / 51mm APAC", 4536, true) \
	MODEL("fēnix® 8 47mm / 51mm / tactix® 8 47mm / 51mm / quatix® 8 47mm / 51mm", 4775, true) \
	MODEL("fēnix® 8 Pro 47mm / 51mm / MicroLED / quatix® 8 Pro 47mm / 51mm", 4631, true) \
	MODEL("fēnix® 8 Solar 47mm", 4532, true) \
	MODEL("fēnix® 8 Solar 51mm / tactix® 8 Solar 51mm APAC", 4533, true) \
	MODEL("fēnix® 8 Solar 51mm / tactix® 8 Solar 51mm", 4776, true)

extern const garmin_model_t garmin_models[];

#endif /* GARMIN_MODELS_H */
