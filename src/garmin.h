/*
 * Garmin Descent Mk1
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

#ifndef GARMIN_H
#define GARMIN_H

#include <libdivecomputer/context.h>
#include <libdivecomputer/iostream.h>
#include <libdivecomputer/device.h>
#include <libdivecomputer/parser.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

dc_status_t
garmin_device_open (dc_device_t **device, dc_context_t *context, dc_iostream_t *iostream, unsigned int model);

dc_status_t
garmin_parser_create (dc_parser_t **parser, dc_context_t *context, const unsigned char data[], size_t size);

// we need to be able to call into the parser to check if the
// files that we find are actual dives
int
garmin_parser_is_dive (dc_parser_t *abstract, dc_event_devinfo_t *devinfo_p);

// The dive names are of the form "2018-08-20-10-23-30.fit"
// With the terminating zero, that's 24 bytes.
//
// We use this as the fingerprint, but it ends up being a
// special fixed header in the parser data too.
#define FIT_NAME_SIZE 24

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* GARMIN_H */
