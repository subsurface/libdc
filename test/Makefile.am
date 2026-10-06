AM_CPPFLAGS = -I$(top_builddir)/include -I$(top_srcdir)/include
LDADD = $(top_builddir)/src/libdivecomputer.la

check_PROGRAMS = test_descriptor_product_name

test_descriptor_product_name_SOURCES = test_descriptor_product_name.c

TESTS = test_descriptor_product_name

EXTRA_DIST = \
	run-parse.sh \
	fixtures/manifest.txt \
	fixtures/shearwater_petrel2-0001.bin \
	fixtures/hw_ostc5-0001.bin \
	fixtures/garmin_descent_mk1-0001.bin
