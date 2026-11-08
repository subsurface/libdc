// AI-generated (Claude)
// Parses the Perdix 2 fixture, a dive with a sidemount pair on 21% (T1, T2)
// and stages on 50% (T3) and oxygen (T4), with the stage valves closed until
// the gas switch, and checks the tanks the parser reports.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libdivecomputer/context.h>
#include <libdivecomputer/descriptor.h>
#include <libdivecomputer/parser.h>

#define NTANKS 4

int
main (void)
{
	// The gas mixes are 99%, 50% and 21%; diluent slot 5 is filtered out.
	const unsigned int expected_gasmix[NTANKS] = {2, 2, 1, 0};
	// The highest readings, in bar. T3 and T4 first read 179 and 115 bar.
	const double expected_begin[NTANKS] = {220.0, 232.0, 207.0, 162.0};
	int failures = 0;

	const char *srcdir = getenv ("srcdir");
	char path[1024];
	snprintf (path, sizeof (path), "%s/fixtures/shearwater_perdix2-0001.bin", srcdir ? srcdir : ".");
	FILE *fp = fopen (path, "rb");
	if (fp == NULL) {
		fprintf (stderr, "FAIL: cannot open %s\n", path);
		return 1;
	}
	static unsigned char data[64 * 1024];
	size_t size = fread (data, 1, sizeof (data), fp);
	fclose (fp);

	dc_context_t *context = NULL;
	dc_context_new (&context);
	dc_iterator_t *iterator = NULL;
	dc_descriptor_t *descriptor = NULL, *d = NULL;
	dc_descriptor_iterator_new (&iterator, context);
	while (dc_iterator_next (iterator, &d) == DC_STATUS_SUCCESS) {
		if (strcmp (dc_descriptor_get_vendor (d), "Shearwater") == 0 &&
		    strcmp (dc_descriptor_get_product (d), "Perdix 2") == 0) {
			descriptor = d;
			break;
		}
		dc_descriptor_free (d);
	}
	dc_iterator_free (iterator);
	if (descriptor == NULL) {
		fprintf (stderr, "FAIL: no Perdix 2 descriptor\n");
		return 1;
	}

	dc_parser_t *parser = NULL;
	if (dc_parser_new2 (&parser, context, descriptor, data, size) != DC_STATUS_SUCCESS) {
		fprintf (stderr, "FAIL: cannot create the parser\n");
		return 1;
	}

	unsigned int ngasmixes = 0, ntanks = 0;
	dc_parser_get_field (parser, DC_FIELD_GASMIX_COUNT, 0, &ngasmixes);
	dc_parser_get_field (parser, DC_FIELD_TANK_COUNT, 0, &ntanks);
	if (ngasmixes != 3 || ntanks != NTANKS) {
		fprintf (stderr, "FAIL: %u gas mixes and %u tanks, expected 3 and %u\n", ngasmixes, ntanks, NTANKS);
		failures++;
	} else {
		for (unsigned int i = 0; i < NTANKS; ++i) {
			dc_tank_t tank = {0};
			dc_parser_get_field (parser, DC_FIELD_TANK, i, &tank);
			if (tank.gasmix != expected_gasmix[i]) {
				fprintf (stderr, "FAIL: tank %u on gas mix %d, expected %u\n", i, (int) tank.gasmix, expected_gasmix[i]);
				failures++;
			}
			if (tank.beginpressure < expected_begin[i] - 1.0 || tank.beginpressure > expected_begin[i] + 1.0) {
				fprintf (stderr, "FAIL: tank %u begin pressure %.1f bar, expected %.0f bar\n", i, tank.beginpressure, expected_begin[i]);
				failures++;
			}
		}
	}

	dc_parser_destroy (parser);
	dc_descriptor_free (descriptor);
	dc_context_free (context);

	return failures ? 1 : 0;
}
