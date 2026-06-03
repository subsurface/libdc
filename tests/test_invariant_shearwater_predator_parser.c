#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <libdivecomputer/parser.h>
#include <libdivecomputer/context.h>

/* We need to exercise the real parser through the public API */
#include "src/shearwater_predator_parser.c"

START_TEST(test_shearwater_predator_oob_memcpy)
{
    /* Invariant: Parser must not read beyond the data buffer boundary,
       even when offset values would place memcpy sources past buffer end. */

    /* Test cases: various buffer sizes that are too small for the offsets used */
    struct {
        unsigned int size;
        unsigned char fill;
    } cases[] = {
        { 0, 0x00 },    /* Empty buffer - no data at all */
        { 16, 0x41 },   /* Buffer smaller than minimum required offsets */
        { 25, 0x42 },   /* Just barely too small (offset+23 + 3 bytes needed) */
        { 128, 0x00 },  /* Valid-sized buffer with zeroed content */
    };
    int num_cases = sizeof(cases) / sizeof(cases[0]);

    for (int i = 0; i < num_cases; i++) {
        dc_context_t *context = NULL;
        dc_parser_t *parser = NULL;

        dc_context_new(&context);

        unsigned char *data = NULL;
        if (cases[i].size > 0) {
            data = (unsigned char *)malloc(cases[i].size);
            ck_assert_ptr_nonnull(data);
            memset(data, cases[i].fill, cases[i].size);
        }

        dc_status_t rc = shearwater_predator_parser_create(&parser, context, 0);
        if (rc == DC_STATUS_SUCCESS && parser != NULL) {
            /* Setting data should not crash or read OOB */
            rc = dc_parser_set_data(parser, data, cases[i].size);
            /* Even if set_data succeeds, getting tank info should not crash */
            if (rc == DC_STATUS_SUCCESS) {
                dc_gasmix_t gasmix;
                dc_parser_get_field(parser, DC_FIELD_GASMIX, 0, &gasmix);
            }
            dc_parser_destroy(parser);
        }

        free(data);
        dc_context_free(context);
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_shearwater_predator_oob_memcpy);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}