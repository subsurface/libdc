// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2020 Linus Torvalds

#ifndef OCEANS_S1_H
#define OCEANS_S1_H

#include <libdivecomputer/context.h>
#include <libdivecomputer/iostream.h>
#include <libdivecomputer/device.h>
#include <libdivecomputer/parser.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

dc_status_t
oceans_s1_device_open (dc_device_t **device, dc_context_t *context, dc_iostream_t *iostream);

dc_status_t
oceans_s1_parser_create (dc_parser_t **parser, dc_context_t *context);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* OCEANS_S1_H */
