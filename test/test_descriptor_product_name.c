/*
 * libdivecomputer
 *
 * Copyright (C) 2024 The libdivecomputer contributors
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

/*
 * Unit tests for dc_descriptor_find_by_product_name().
 *
 * Covers:
 *   - Mares Puck Pro vs Puck Pro + (shared model 0x18, PUCKPRO): the
 *     firmware name "Puck Pro" resolves to the "Puck Pro" descriptor.
 *   - Mares Puck 4 family (shared model 0x35, PUCK4): "Puck4", "Puck Lite",
 *     and "Puck Pro U" each resolve to the correct distinct descriptor.
 *   - Coarse-model fallback: an unrecognised product name returns NULL.
 *   - NULL / empty product name returns NULL.
 */

#include <stdio.h>
#include <string.h>

#include <libdivecomputer/descriptor.h>
#include <libdivecomputer/common.h>

#define PUCKPRO_MODEL  0x18
#define PUCK4_MODEL    0x35

static int failures = 0;

static void
check (const char *test, int condition)
{
	if (!condition) {
		fprintf (stderr, "FAIL: %s\n", test);
		failures++;
	} else {
		printf ("PASS: %s\n", test);
	}
}

int main (void)
{
	dc_descriptor_t *desc;

	/* --- Puck Pro (model 0x18) --- */

	/* "Puck Pro" firmware name must resolve to the "Puck Pro" descriptor. */
	desc = dc_descriptor_find_by_product_name (DC_FAMILY_MARES_ICONHD, PUCKPRO_MODEL, "Puck Pro");
	check ("Puck Pro firmware name resolves to a descriptor", desc != NULL);
	if (desc != NULL) {
		check ("Puck Pro descriptor product is 'Puck Pro'",
			strcmp (dc_descriptor_get_product (desc), "Puck Pro") == 0);
		check ("Puck Pro descriptor model is 0x18",
			dc_descriptor_get_model (desc) == PUCKPRO_MODEL);
		dc_descriptor_free (desc);
	}

	/* "Puck Pro +" has no confirmed firmware product-name; must return NULL
	 * so the caller falls back to the coarse-model descriptor.  We pass the
	 * descriptor product name as the query to verify it is not in the table. */
	desc = dc_descriptor_find_by_product_name (DC_FAMILY_MARES_ICONHD, PUCKPRO_MODEL, "Puck Pro +");
	check ("'Puck Pro +' (not in firmware table) returns NULL", desc == NULL);

	/* --- Puck 4 family (model 0x35) --- */

	/* "Puck4" must resolve to "Puck 4". */
	desc = dc_descriptor_find_by_product_name (DC_FAMILY_MARES_ICONHD, PUCK4_MODEL, "Puck4");
	check ("'Puck4' firmware name resolves to a descriptor", desc != NULL);
	if (desc != NULL) {
		check ("'Puck4' descriptor product is 'Puck 4'",
			strcmp (dc_descriptor_get_product (desc), "Puck 4") == 0);
		check ("'Puck4' descriptor model is 0x35",
			dc_descriptor_get_model (desc) == PUCK4_MODEL);
		dc_descriptor_free (desc);
	}

	/* "Puck Lite" must resolve to "Puck Lite". */
	desc = dc_descriptor_find_by_product_name (DC_FAMILY_MARES_ICONHD, PUCK4_MODEL, "Puck Lite");
	check ("'Puck Lite' firmware name resolves to a descriptor", desc != NULL);
	if (desc != NULL) {
		check ("'Puck Lite' descriptor product is 'Puck Lite'",
			strcmp (dc_descriptor_get_product (desc), "Puck Lite") == 0);
		dc_descriptor_free (desc);
	}

	/* "Puck Pro U" must resolve to "Puck Pro Ultra". */
	desc = dc_descriptor_find_by_product_name (DC_FAMILY_MARES_ICONHD, PUCK4_MODEL, "Puck Pro U");
	check ("'Puck Pro U' firmware name resolves to a descriptor", desc != NULL);
	if (desc != NULL) {
		check ("'Puck Pro U' descriptor product is 'Puck Pro Ultra'",
			strcmp (dc_descriptor_get_product (desc), "Puck Pro Ultra") == 0);
		dc_descriptor_free (desc);
	}

	/* --- Coarse-model fallback cases --- */

	/* "Puck" maps to model 0x35 in the driver but is ambiguous; must return NULL. */
	desc = dc_descriptor_find_by_product_name (DC_FAMILY_MARES_ICONHD, PUCK4_MODEL, "Puck");
	check ("Ambiguous 'Puck' firmware name returns NULL", desc == NULL);

	/* Completely unknown product name returns NULL. */
	desc = dc_descriptor_find_by_product_name (DC_FAMILY_MARES_ICONHD, PUCK4_MODEL, "Unknown Product");
	check ("Unknown product name returns NULL", desc == NULL);

	/* NULL product name returns NULL. */
	desc = dc_descriptor_find_by_product_name (DC_FAMILY_MARES_ICONHD, PUCK4_MODEL, NULL);
	check ("NULL product name returns NULL", desc == NULL);

	/* Empty product name returns NULL. */
	desc = dc_descriptor_find_by_product_name (DC_FAMILY_MARES_ICONHD, PUCK4_MODEL, "");
	check ("Empty product name returns NULL", desc == NULL);

	/* Wrong family returns NULL even with a valid name. */
	desc = dc_descriptor_find_by_product_name (DC_FAMILY_MARES_PUCK, PUCK4_MODEL, "Puck4");
	check ("Wrong family returns NULL", desc == NULL);

	if (failures == 0) {
		printf ("All tests passed.\n");
		return 0;
	}

	fprintf (stderr, "%d test(s) failed.\n", failures);
	return 1;
}
