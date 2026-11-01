/* Synthetic Symbios records: no device logs or identifying data. */
#include <stdio.h>

#include <libdivecomputer/descriptor.h>
#include <libdivecomputer/iterator.h>
#include <libdivecomputer/parser.h>

#define GAS(id, o2, he, enabled) 0x11, 8, id, o2, he, enabled, 0, 0
#define TRANSMITTER(id, serial) 0x06, 12, id, 0, 0, 0, 0xD2, 0x04, serial, 0, 0, 0

typedef struct samples_t {
	unsigned int gases[16];
	unsigned int ngases;
	unsigned int pressures[16];
	unsigned int npressures;
} samples_t;

static int failures = 0;

static dc_descriptor_t *
handset_descriptor (void)
{
	dc_iterator_t *iterator = NULL;
	dc_descriptor_t *descriptor = NULL;
	dc_descriptor_t *item = NULL;
	if (dc_descriptor_iterator (&iterator) != DC_STATUS_SUCCESS)
		return NULL;
	while (dc_iterator_next (iterator, &item) == DC_STATUS_SUCCESS) {
		if (dc_descriptor_get_type (item) == DC_FAMILY_HALCYON_SYMBIOS &&
			dc_descriptor_get_model (item) == 7) {
			descriptor = item;
			break;
		}
	}
	dc_iterator_free (iterator);
	return descriptor;
}

static void
check (const char *name, int condition)
{
	if (!condition) {
		fprintf (stderr, "FAIL: %s\n", name);
		failures++;
	}
}

static void
sample (dc_sample_type_t type, const dc_sample_value_t *value, void *userdata)
{
	samples_t *samples = userdata;
	if (type == DC_SAMPLE_GASMIX && samples->ngases < 16)
		samples->gases[samples->ngases++] = value->gasmix;
	if (type == DC_SAMPLE_PRESSURE && samples->npressures < 16)
		samples->pressures[samples->npressures++] = value->pressure.tank;
}

static void
check_mix (dc_parser_t *parser, unsigned int index, unsigned int oxygen, unsigned int helium)
{
	dc_gasmix_t mix;
	check ("mix field", dc_parser_get_field (parser, DC_FIELD_GASMIX, index, &mix) == DC_STATUS_SUCCESS);
	check ("oxygen fraction", mix.oxygen == oxygen / 100.0);
	check ("helium fraction", mix.helium == helium / 100.0);
}

static void
check_profile (const unsigned char *data, size_t size, unsigned int expected_count,
	const unsigned int *expected_samples, unsigned int nsamples)
{
	dc_parser_t *parser = NULL;
	dc_descriptor_t *descriptor = handset_descriptor ();
	samples_t samples = {{0}, 0, {0}, 0};
	unsigned int count = 0;
	check ("handset descriptor", descriptor != NULL);
	if (descriptor == NULL)
		return;
	check ("create parser", dc_parser_new2 (&parser, NULL, descriptor, data, size) == DC_STATUS_SUCCESS);
	dc_descriptor_free (descriptor);
	if (parser == NULL)
		return;
	check ("parse samples", dc_parser_samples_foreach (parser, sample, &samples) == DC_STATUS_SUCCESS);
	check ("gas count field", dc_parser_get_field (parser, DC_FIELD_GASMIX_COUNT, 0, &count) == DC_STATUS_SUCCESS);
	check ("gas count", count == expected_count);
	check ("sample count", samples.ngases == nsamples);
	for (unsigned int i = 0; i < nsamples && i < samples.ngases; ++i)
		check ("sample gas index", samples.gases[i] == expected_samples[i]);
	if (expected_count == 4) {
		check_mix (parser, 0, 21, 0);
		check_mix (parser, 1, 21, 0);
		check_mix (parser, 2, 21, 35);
		check_mix (parser, 3, 18, 45);
	} else if (expected_count == 8) {
		dc_tank_t tank;
		check_mix (parser, 1, 32, 0);
		check_mix (parser, 2, 30, 0); /* Enabled, never selected. */
		check_mix (parser, 3, 21, 0);
		check_mix (parser, 6, 40, 0);
		check ("tank count", dc_parser_get_field (parser, DC_FIELD_TANK_COUNT, 0, &count) == DC_STATUS_SUCCESS && count == 2);
		check ("first tank", dc_parser_get_field (parser, DC_FIELD_TANK, 0, &tank) == DC_STATUS_SUCCESS && tank.gasmix == 1);
		check ("second tank", dc_parser_get_field (parser, DC_FIELD_TANK, 1, &tank) == DC_STATUS_SUCCESS && tank.gasmix == 3);
		check ("pressure tank indices", samples.npressures == 2 && samples.pressures[0] == 0 && samples.pressures[1] == 1);
	}
	check ("destroy parser", dc_parser_destroy (parser) == DC_STATUS_SUCCESS);
}

int
main (void)
{
	/* Native-export pattern: disabled GAS2-5, enabled GAS1, unchanged diluents. */
	static const unsigned char native_pattern[] = {
		GAS (0, 21, 0, 1), GAS (1, 50, 0, 0), GAS (2, 99, 0, 0),
		GAS (3, 21, 35, 0), GAS (4, 18, 45, 0), GAS (5, 21, 0, 1),
		GAS (6, 21, 35, 0), GAS (7, 18, 45, 0), GAS (0, 21, 0, 1),
		GAS (1, 50, 0, 0), GAS (2, 0, 0, 1)
	};
	static const unsigned int native_samples[] = {0};
	/* Non-boolean nonzero enable value; gas 2 remains visible without selection. */
	static const unsigned char linkage[] = {
		GAS (0, 21, 0, 1), GAS (1, 50, 0, 0), GAS (2, 32, 0, 2),
		GAS (4, 30, 0, 2),
		GAS (5, 21, 0, 0), GAS (6, 21, 35, 1), GAS (7, 18, 45, 0),
		GAS (8, 40, 0, 0), GAS (1, 50, 0, 0), GAS (0, 21, 0, 1),
		TRANSMITTER (2, 1), TRANSMITTER (5, 2),
		0x02, 4, 36, 0 /* Gas-switch record retains its independent path. */
	};
	static const unsigned int linkage_samples[] = {0, 1, 3, 7};
	dc_parser_t *parser = NULL;
	dc_descriptor_t *descriptor = handset_descriptor ();
	static const unsigned char short_config[] = {0x11, 7, 0, 21, 0, 1, 0};

	check_profile (native_pattern, sizeof (native_pattern), 4, native_samples, 1);
	check_profile (linkage, sizeof (linkage), 8, linkage_samples, 4);
	check ("create short parser", dc_parser_new2 (&parser, NULL, descriptor, short_config, sizeof (short_config)) == DC_STATUS_SUCCESS);
	dc_descriptor_free (descriptor);
	if (parser) {
		check ("reject short gas config", dc_parser_samples_foreach (parser, NULL, NULL) == DC_STATUS_DATAFORMAT);
		dc_parser_destroy (parser);
	}
	return failures != 0;
}
