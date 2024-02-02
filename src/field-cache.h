#include <string.h>

#define MAXGASES 16
#define MAXSTRINGS 32

// dc_get_field() data
typedef struct dc_field_cache {
	unsigned int initialized;

	// DC_GET_FIELD_xyz
	unsigned int DIVETIME;
	double MAXDEPTH;
	double AVGDEPTH;
	double ATMOSPHERIC;
	dc_divemode_t DIVEMODE;
	unsigned int GASMIX_COUNT;
	dc_salinity_t SALINITY;
	dc_gasmix_t GASMIX[MAXGASES];

	// misc - clean me up!
	double lowsetpoint;
	double highsetpoint;
	double customsetpoint;

	// This (along with GASMIX) should be something like
	//     dc_tank_t TANK[MAXGASES]
	// but that's for later
	dc_tankinfo_t tankinfo[MAXGASES];
	dc_usage_t tankusage[MAXGASES];
	double tanksize[MAXGASES];
	double tankworkingpressure[MAXGASES];

	// DC_GET_FIELD_STRING
	dc_field_string_t strings[MAXSTRINGS];
} dc_field_cache_t;

dc_status_t dc_field_add_string(dc_field_cache_t *, const char *desc, const char *data);
dc_status_t dc_field_add_string_fmt(dc_field_cache_t *, const char *desc, const char *fmt, ...);
dc_status_t dc_field_get_string(dc_field_cache_t *, unsigned idx, dc_field_string_t *value);
dc_status_t dc_field_get(dc_field_cache_t *, dc_field_type_t, unsigned int, void *);

/*
 * Macro to make it easy to set DC_FIELD_xyz values.
 *
 * This explains why dc_field_cache member names are
 * those odd all-capitalized names: they match the
 * names of the DC_FIELD_xyz enums.
 */
#define DC_ASSIGN_FIELD(cache, name, value) do { \
	(cache).initialized |= 1u << DC_FIELD_##name; \
	(cache).name = (value); \
} while (0)

#define DC_ASSIGN_IDX(cache, name, idx, value) do { \
	(cache).initialized |= 1u << DC_FIELD_##name; \
	(cache).name[idx] = (value); \
} while (0)

// Ugly define thing makes the code much easier to read
// I'd love to use __typeof__, but that's a gcc'ism
#define DC_FIELD_VALUE(cache, p, NAME) \
	(memcpy((p), &(cache).NAME, sizeof((cache).NAME)), DC_STATUS_SUCCESS)

#define DC_FIELD_INDEX(cache, p, NAME, idx) \
	(memcpy((p), (cache).NAME+idx, sizeof((cache).NAME[0])), DC_STATUS_SUCCESS)
