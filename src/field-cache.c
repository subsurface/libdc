#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "parser-private.h"
#include "field-cache.h"

/*
 * The field cache 'string' interface has some simple rules:
 * the "descriptor" part is assumed to be a static allocation,
 * while the "value" is something that this interface will
 * always allocate with 'strdup()', so you can generate it
 * dynamically on the stack or whatever without having to
 * worry about it.
 */
dc_status_t dc_field_add_string(dc_field_cache_t *cache, const char *desc, const char *value)
{
	int i;

	cache->initialized |= 1 << DC_FIELD_STRING;
	for (i = 0; i < MAXSTRINGS; i++) {
		dc_field_string_t *str = cache->strings+i;
		if (str->desc)
			continue;
		str->value = strdup(value);
		if (!str->value)
			return DC_STATUS_NOMEMORY;
		str->desc = desc;
		return DC_STATUS_SUCCESS;
	}
	return DC_STATUS_INVALIDARGS;
}

dc_status_t dc_field_add_string_fmt(dc_field_cache_t *cache, const char *desc, const char *fmt, ...)
{
	char buffer[256];
	va_list ap;

	/*
	 * We ignore the return value from vsnprintf, and we
	 * always NUL-terminate the destination buffer ourselves.
	 *
	 * That way we don't have to worry about random bad legacy
	 * implementations.
	 */
	va_start(ap, fmt);
	buffer[sizeof(buffer)-1] = 0;
	(void) vsnprintf(buffer, sizeof(buffer)-1, fmt, ap);
	va_end(ap);

	return dc_field_add_string(cache, desc, buffer);
}

dc_status_t dc_field_get_string(dc_field_cache_t *cache, unsigned idx, dc_field_string_t *value)
{
	if (idx < MAXSTRINGS) {
		dc_field_string_t *res = cache->strings+idx;
		if (res->desc && res->value) {
			*value = *res;
			return DC_STATUS_SUCCESS;
		}
	}
	return DC_STATUS_UNSUPPORTED;
}


/*
 * Use this generic "pick fields from the field cache" helper
 * after you've handled all the ones you do differently
 */
dc_status_t
dc_field_get(dc_field_cache_t *cache, dc_field_type_t type, unsigned int flags, void* value)
{
	if (!value)
		return DC_STATUS_INVALIDARGS;

	if (!(cache->initialized & (1 << type)))
		return DC_STATUS_UNSUPPORTED;

	switch (type) {
	case DC_FIELD_DIVETIME:
		return DC_FIELD_VALUE(*cache, value, DIVETIME);
	case DC_FIELD_MAXDEPTH:
		return DC_FIELD_VALUE(*cache, value, MAXDEPTH);
	case DC_FIELD_AVGDEPTH:
		return DC_FIELD_VALUE(*cache, value, AVGDEPTH);
	case DC_FIELD_GASMIX_COUNT:
	case DC_FIELD_TANK_COUNT:
		return DC_FIELD_VALUE(*cache, value, GASMIX_COUNT);
	case DC_FIELD_GASMIX:
		if (flags >= MAXGASES)
			break;
		return DC_FIELD_INDEX(*cache, value, GASMIX, flags);
	case DC_FIELD_SALINITY:
		return DC_FIELD_VALUE(*cache, value, SALINITY);
	case DC_FIELD_DIVEMODE:
		return DC_FIELD_VALUE(*cache, value, DIVEMODE);
	case DC_FIELD_STRING:
		return dc_field_get_string(cache, flags, (dc_field_string_t *)value);
	default:
		break;
	}

	return DC_STATUS_UNSUPPORTED;
}
