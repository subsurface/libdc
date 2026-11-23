/* Synthetic Symbios records: no device logs or identifying data. */
#include <math.h>
#include <stdio.h>

#include <libdivecomputer/descriptor.h>
#include <libdivecomputer/iterator.h>
#include <libdivecomputer/parser.h>

#define GAS(id, o2, he, enabled) 0x11, 8, id, o2, he, enabled, 0, 0
#define TRANSMITTER(id, serial) 0x06, 12, id, 0, 0, 0, 0xD2, 0x04, serial, 0, 0, 0
/* 12-byte ID_PO2_REBREATHER: three PPO2 sensors, then 3 pad bytes,
 * then pressure (uint16_le, 0.1-bar units), then serial (uint16_le). */
#define PO2_REBREATHER(ppo2, pressure, serial) \
	0x0D, 12, (ppo2), (ppo2), (ppo2), 0, 0, 0, \
	((pressure) & 0xFF), ((pressure) >> 8), \
	((serial) & 0xFF), ((serial) >> 8)

/* Minimal 64-byte ID_HEADER with divemode at byte offset 13.
 * time_start is set to 1 so it is not treated as UNDEFINED. */
#define HEADER(divemode) \
	0x01, 64, \
	0x07, 0x0c, 0x00, 0x02, 0x24, 0x0c, 0x01, 0x00, \
	0x05, 0x00, 0x00, (divemode), 0x00, 0x00, \
	0x0d, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0

/* Divemode values. */
#define DIVEMODE_OC  0
#define DIVEMODE_CCR 1

/* dc_descriptor_get_model() value for the "Symbios Handset", see descriptor.c. */
#define HANDSET_MODEL_SYMBIOS 7

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
			dc_descriptor_get_model (item) == HANDSET_MODEL_SYMBIOS) {
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
	check ("oxygen fraction", fabs (mix.oxygen - oxygen / 100.0) < 1e-9);
	check ("helium fraction", fabs (mix.helium - helium / 100.0) < 1e-9);
}

static void
check_usage (dc_parser_t *parser, unsigned int index, dc_usage_t expected)
{
	dc_gasmix_t mix;
	check ("mix field for usage", dc_parser_get_field (parser, DC_FIELD_GASMIX, index, &mix) == DC_STATUS_SUCCESS);
	check ("gas usage", mix.usage == expected);
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
	/*
	 * Expected gas count of 8: 7 distinct ids registered from "linkage"
	 * (0, 2, 4, 5, 6, 7, 8, at indices 0-6) plus one more gas mix
	 * synthesized by the trailing ID_GAS_SWITCH record, which always uses
	 * its own untagged id and is appended as index 7. Ids 0, 2 and 4 are
	 * registered because they are enabled; ids 5, 7 and 8 are registered
	 * regardless of their enable byte because ids >= NGAS_SLOTS are
	 * diluents, which skip the enable check; id 6 is both enabled and a
	 * diluent.
	 *
	 * Expected sample indices {0, 1, 3, 7}: index 0 comes from the
	 * repeated GAS(0, 21, 0, 1) record re-confirming the already
	 * registered gas 0; index 1 and 3 come from TRANSMITTER(2, 1) and
	 * TRANSMITTER(5, 2) selecting gases 2 and 5; index 7 comes from the
	 * trailing gas-switch record.
	 */
	static const unsigned int linkage_samples[] = {0, 1, 3, 7};
	/* Gas1 is disabled in its config but later reported by a transmitter;
	 * it must resolve instead of being rejected, and only becomes part of
	 * the exposed gas list once referenced. */
	static const unsigned char deferred[] = {
		GAS (0, 21, 0, 1), GAS (1, 32, 0, 0),
		TRANSMITTER (1, 5)
	};
	/* Gas1 is disabled and reconfigured with a different oxygen percentage
	 * before ever being referenced; the conflicting retained configuration
	 * must still be rejected, instead of silently keeping the first one. */
	static const unsigned char deferred_conflict[] = {
		GAS (0, 21, 0, 1), GAS (1, 32, 0, 0), GAS (1, 50, 0, 0)
	};
	/* The only configured gas is disabled, so no enabled or diluent gas is
	 * ever registered; its transmitter must still resolve it instead of
	 * leaving the tank's gas mix unknown. */
	static const unsigned char disabled_only[] = {
		GAS (0, 21, 0, 0), TRANSMITTER (0, 9)
	};
	/* CCR dive: OC bailout (id < NGAS_SLOTS), diluent (id >= NGAS_SLOTS),
	 * and O2 supply tank; parser synthesizes a 100% O2 gasmix. */
	static const unsigned char ccr_data[] = {
		HEADER (DIVEMODE_CCR),
		GAS (0, 21, 0, 1),
		GAS (5, 21, 0, 1),
		PO2_REBREATHER (150, 2000, 42),
	};
	/* OC dive: all slots -> DC_USAGE_NONE regardless of id. */
	static const unsigned char oc_usage_data[] = {
		HEADER (DIVEMODE_OC),
		GAS (0, 21, 0, 1),
		GAS (5, 21, 0, 1),
	};
	dc_parser_t *parser = NULL;
	dc_descriptor_t *descriptor = handset_descriptor ();
	static const unsigned char short_config[] = {0x11, 7, 0, 21, 0, 1, 0};
	dc_parser_t *deferred_parser = NULL;
	dc_descriptor_t *deferred_descriptor = handset_descriptor ();
	unsigned int deferred_count = 0;
	dc_tank_t deferred_tank;
	dc_parser_t *conflict_parser = NULL;
	dc_descriptor_t *conflict_descriptor = handset_descriptor ();
	dc_parser_t *disabled_only_parser = NULL;
	dc_descriptor_t *disabled_only_descriptor = handset_descriptor ();
	unsigned int disabled_only_count = 0;
	dc_tank_t disabled_only_tank;

	check_profile (native_pattern, sizeof (native_pattern), 4, native_samples, 1);
	check_profile (linkage, sizeof (linkage), 8, linkage_samples, 4);
	check ("create short parser", dc_parser_new2 (&parser, NULL, descriptor, short_config, sizeof (short_config)) == DC_STATUS_SUCCESS);
	dc_descriptor_free (descriptor);
	if (parser) {
		check ("reject short gas config", dc_parser_samples_foreach (parser, NULL, NULL) == DC_STATUS_DATAFORMAT);
		dc_parser_destroy (parser);
	}

	check ("handset descriptor (deferred)", deferred_descriptor != NULL);
	if (deferred_descriptor) {
		check ("create deferred parser", dc_parser_new2 (&deferred_parser, NULL, deferred_descriptor, deferred, sizeof (deferred)) == DC_STATUS_SUCCESS);
		dc_descriptor_free (deferred_descriptor);
	}
	if (deferred_parser) {
		check ("resolve deferred transmitter", dc_parser_samples_foreach (deferred_parser, NULL, NULL) == DC_STATUS_SUCCESS);
		check ("deferred gas count field", dc_parser_get_field (deferred_parser, DC_FIELD_GASMIX_COUNT, 0, &deferred_count) == DC_STATUS_SUCCESS);
		check ("deferred gas count", deferred_count == 2);
		check ("deferred tank gasmix", dc_parser_get_field (deferred_parser, DC_FIELD_TANK, 0, &deferred_tank) == DC_STATUS_SUCCESS && deferred_tank.gasmix == 1);
		dc_parser_destroy (deferred_parser);
	}

	check ("handset descriptor (conflict)", conflict_descriptor != NULL);
	if (conflict_descriptor) {
		check ("create conflict parser", dc_parser_new2 (&conflict_parser, NULL, conflict_descriptor, deferred_conflict, sizeof (deferred_conflict)) == DC_STATUS_SUCCESS);
		dc_descriptor_free (conflict_descriptor);
	}
	if (conflict_parser) {
		check ("reject conflicting retained configuration", dc_parser_samples_foreach (conflict_parser, NULL, NULL) == DC_STATUS_DATAFORMAT);
		dc_parser_destroy (conflict_parser);
	}

	check ("handset descriptor (disabled only)", disabled_only_descriptor != NULL);
	if (disabled_only_descriptor) {
		check ("create disabled-only parser", dc_parser_new2 (&disabled_only_parser, NULL, disabled_only_descriptor, disabled_only, sizeof (disabled_only)) == DC_STATUS_SUCCESS);
		dc_descriptor_free (disabled_only_descriptor);
	}
	if (disabled_only_parser) {
		check ("resolve transmitter without a prior enabled gas", dc_parser_samples_foreach (disabled_only_parser, NULL, NULL) == DC_STATUS_SUCCESS);
		check ("disabled-only gas count field", dc_parser_get_field (disabled_only_parser, DC_FIELD_GASMIX_COUNT, 0, &disabled_only_count) == DC_STATUS_SUCCESS);
		check ("disabled-only gas count", disabled_only_count == 1);
		check ("disabled-only tank gasmix", dc_parser_get_field (disabled_only_parser, DC_FIELD_TANK, 0, &disabled_only_tank) == DC_STATUS_SUCCESS && disabled_only_tank.gasmix == 0);
		dc_parser_destroy (disabled_only_parser);
	}

	/* CCR: OC bailout, diluent, and synthetic O2 gasmix linked to tank. */
	{
		dc_parser_t *ccr_parser = NULL;
		dc_descriptor_t *ccr_desc = handset_descriptor ();
		unsigned int ccr_count = 0;
		dc_gasmix_t o2_mix;
		dc_tank_t o2_tank;
		check ("handset descriptor (ccr)", ccr_desc != NULL);
		if (ccr_desc) {
			check ("create ccr parser", dc_parser_new2 (&ccr_parser, NULL, ccr_desc, ccr_data, sizeof (ccr_data)) == DC_STATUS_SUCCESS);
			dc_descriptor_free (ccr_desc);
		}
		if (ccr_parser) {
			check ("parse ccr samples", dc_parser_samples_foreach (ccr_parser, NULL, NULL) == DC_STATUS_SUCCESS);
			check_usage (ccr_parser, 0, DC_USAGE_OPEN_CIRCUIT);  /* id=0, OC bailout */
			check_usage (ccr_parser, 1, DC_USAGE_DILUENT);       /* id=5, CCR diluent */
			check ("ccr gasmix count field", dc_parser_get_field (ccr_parser, DC_FIELD_GASMIX_COUNT, 0, &ccr_count) == DC_STATUS_SUCCESS);
			check ("ccr gasmix count", ccr_count == 3);
			check ("o2 gasmix field", dc_parser_get_field (ccr_parser, DC_FIELD_GASMIX, 2, &o2_mix) == DC_STATUS_SUCCESS);
			check ("o2 gasmix oxygen", o2_mix.oxygen == 1.0);
			check ("o2 gasmix usage", o2_mix.usage == DC_USAGE_OXYGEN);
			check ("o2 tank field", dc_parser_get_field (ccr_parser, DC_FIELD_TANK, 0, &o2_tank) == DC_STATUS_SUCCESS);
			check ("o2 tank gasmix link", o2_tank.gasmix == 2);
			dc_parser_destroy (ccr_parser);
		}
	}

	/* OC: all slots -> DC_USAGE_NONE regardless of id. */
	{
		dc_parser_t *oc_parser = NULL;
		dc_descriptor_t *oc_desc = handset_descriptor ();
		check ("handset descriptor (oc)", oc_desc != NULL);
		if (oc_desc) {
			check ("create oc parser", dc_parser_new2 (&oc_parser, NULL, oc_desc, oc_usage_data, sizeof (oc_usage_data)) == DC_STATUS_SUCCESS);
			dc_descriptor_free (oc_desc);
		}
		if (oc_parser) {
			check ("parse oc samples", dc_parser_samples_foreach (oc_parser, NULL, NULL) == DC_STATUS_SUCCESS);
			check_usage (oc_parser, 0, DC_USAGE_NONE);  /* id=0, OC slot */
			check_usage (oc_parser, 1, DC_USAGE_NONE);  /* id=5, not diluent in OC mode */
			dc_parser_destroy (oc_parser);
		}
	}

	return failures != 0;
}
