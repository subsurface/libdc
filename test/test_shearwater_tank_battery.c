// AI-generated (Claude)
/*
 * Synthetic Shearwater PNF logs: no device logs or identifying data.
 *
 * Checks the per-transmitter battery strings ("T1 battery" to "T4 battery").
 * T1/T2 come from the regular sample record and T3/T4 from the extended
 * sample record (log version 13 and later). Each tank pressure word holds
 * the battery level in the top 4 bits (0 = normal, 1 = critical,
 * 2 = warning) and the pressure in the lower 12 bits; values from 0xFFF0
 * upward are special codes.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <libdivecomputer/descriptor.h>
#include <libdivecomputer/iterator.h>
#include <libdivecomputer/parser.h>

#define SZ_RECORD 32
#define NRECORDS_MAX 16

/* dc_descriptor_get_model() value for the "Perdix 2", see descriptor.c. */
#define MODEL_PERDIX2 11

#define AI_HPCCR 4

/* Pressure word: battery level in the top 4 bits, pressure (2 psi units) below. */
#define WORD(battery, pressure) (((battery) << 12) | (pressure))
#define NORMAL   0
#define CRITICAL 1
#define WARNING  2

typedef struct log_t {
	unsigned char data[NRECORDS_MAX * SZ_RECORD];
	unsigned int nrecords;
} log_t;

static int failures = 0;

/* Count a failure, and report it, if the condition is false. */
static void
check (const char *name, int condition)
{
	if (!condition) {
		fprintf (stderr, "FAIL: %s\n", name);
		failures++;
	}
}

/* Append an empty 32 byte record of the given type and return it. */
static unsigned char *
add_record (log_t *log, unsigned char type)
{
	unsigned char *record = log->data + log->nrecords++ * SZ_RECORD;
	memset (record, 0, SZ_RECORD);
	record[0] = type;
	return record;
}

/* Store a 16 bit value big endian, as in the Shearwater logs. */
static void
put_word (unsigned char *p, unsigned int value)
{
	p[0] = (value >> 8) & 0xFF;
	p[1] = value & 0xFF;
}

/*
 * Build a minimal PNF log: opening records 0-4, one regular sample with the
 * T1/T2 words, one extended sample with the T3/T4 words (both omitted when
 * the log version predates them), and closing records 0-4.
 */
static void
build_log (log_t *log, unsigned int logversion, unsigned int aimode,
	unsigned int t1, unsigned int t2, unsigned int t3, unsigned int t4)
{
	log->nrecords = 0;
	for (unsigned char type = 0x10; type <= 0x13; ++type)
		add_record (log, type);
	unsigned char *opening4 = add_record (log, 0x14);
	opening4[16] = logversion;
	opening4[28] = aimode;

	unsigned char *sample = add_record (log, 0x01);
	sample[1 + 11] = 0x10; /* Status: open circuit. */
	put_word (sample + 1 + 27, t1);
	put_word (sample + 1 + 19, t2);

	if (logversion >= 13) {
		unsigned char *ext = add_record (log, 0xE1);
		put_word (ext + 1, t3);
		put_word (ext + 3, t4);
	}

	for (unsigned char type = 0x20; type <= 0x24; ++type)
		add_record (log, type);
}

/* Find the Perdix 2 descriptor, or return NULL if it is missing. */
static dc_descriptor_t *
perdix2_descriptor (void)
{
	dc_iterator_t *iterator = NULL;
	dc_descriptor_t *descriptor = NULL;
	dc_descriptor_t *item = NULL;
	if (dc_descriptor_iterator (&iterator) != DC_STATUS_SUCCESS)
		return NULL;
	while (dc_iterator_next (iterator, &item) == DC_STATUS_SUCCESS) {
		if (dc_descriptor_get_type (item) == DC_FAMILY_SHEARWATER_PETREL &&
			dc_descriptor_get_model (item) == MODEL_PERDIX2) {
			descriptor = item;
			break;
		}
	}
	dc_iterator_free (iterator);
	return descriptor;
}

/*
 * Check the battery strings reported for T1 to T4. A NULL expected value
 * means the string must be absent.
 */
static void
check_batteries (const char *name, const log_t *log, const char *expected[4])
{
	static const char *desc[4] = {
		"T1 battery", "T2 battery", "T3 battery", "T4 battery",
	};
	char *found[4] = {NULL, NULL, NULL, NULL};
	unsigned int other = 0;
	dc_parser_t *parser = NULL;
	dc_descriptor_t *descriptor = perdix2_descriptor ();

	check ("Perdix 2 descriptor", descriptor != NULL);
	if (descriptor == NULL)
		return;
	check ("create parser", dc_parser_new2 (&parser, NULL, descriptor, log->data, log->nrecords * SZ_RECORD) == DC_STATUS_SUCCESS);
	dc_descriptor_free (descriptor);
	if (parser == NULL)
		return;

	for (unsigned int i = 0; ; ++i) {
		dc_field_string_t str;
		if (dc_parser_get_field (parser, DC_FIELD_STRING, i, &str) != DC_STATUS_SUCCESS)
			break;
		int matched = 0;
		for (unsigned int t = 0; t < 4; ++t) {
			if (strcmp (str.desc, desc[t]) == 0) {
				free (found[t]);
				found[t] = (char *) str.value;
				matched = 1;
			}
		}
		if (!matched) {
			/* An HP CCR slot must not show up as "T5 battery" or "T6 battery". */
			if (strcmp (str.desc, "T5 battery") == 0 || strcmp (str.desc, "T6 battery") == 0)
				other++;
			free ((void *) str.value);
		}
	}

	for (unsigned int t = 0; t < 4; ++t) {
		int ok = expected[t] == NULL ? found[t] == NULL :
			found[t] != NULL && strcmp (found[t], expected[t]) == 0;
		if (!ok) {
			fprintf (stderr, "FAIL: %s: %s is \"%s\", expected \"%s\"\n", name, desc[t],
				found[t] ? found[t] : "(absent)", expected[t] ? expected[t] : "(absent)");
			failures++;
		}
		free (found[t]);
	}
	check ("no battery string beyond T4", other == 0);
	check ("destroy parser", dc_parser_destroy (parser) == DC_STATUS_SUCCESS);
}

/* Run each synthetic log through the parser and check its battery strings. */
int
main (void)
{
	log_t log;

	/* T1/T2 in the regular sample record, T3/T4 in the extended one, covering each state. */
	build_log (&log, 17, 0,
		WORD (NORMAL, 1000), WORD (CRITICAL, 1000),
		WORD (WARNING, 1000), WORD (NORMAL, 1000));
	check_batteries ("four transmitters", &log,
		(const char *[4]) {"normal", "critical", "warning", "normal"});

	/* Special codes (no comms, not paired) and a zero pressure report no state. */
	build_log (&log, 17, 0,
		WORD (NORMAL, 1000), 0xFFFC,
		0xFFFE, WORD (WARNING, 0));
	check_batteries ("special codes and zero pressure", &log,
		(const char *[4]) {"normal", NULL, NULL, NULL});

	/* Before log version 13 there is no extended record, so only T1/T2. */
	build_log (&log, 12, 0,
		WORD (WARNING, 1000), WORD (NORMAL, 1000), 0, 0);
	check_batteries ("log version 12", &log,
		(const char *[4]) {"warning", "normal", NULL, NULL});

	/*
	 * HP CCR in an older log: the regular sample words belong to the HP CCR
	 * slots (indices 4 and 5), which carry no battery state to report.
	 */
	build_log (&log, 12, AI_HPCCR,
		WORD (WARNING, 1000), WORD (WARNING, 1000), 0, 0);
	check_batteries ("HP CCR, log version 12", &log,
		(const char *[4]) {NULL, NULL, NULL, NULL});

	return failures != 0;
}
