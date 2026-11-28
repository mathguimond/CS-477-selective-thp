#define _POSIX_C_SOURCE 200809L

#include "policy.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARSE_FAIL(...)                                                 \
	do {                                                            \
		if (err && errlen)                                      \
			snprintf(err, errlen, __VA_ARGS__);             \
		free(buf);                                              \
		return -EINVAL;                                         \
	} while (0)

static char *trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

static int parse_u64(const char *s, uint64_t *out)
{
	char *end;
	unsigned long long v;

	if (!isdigit((unsigned char)*s))
		return -1;
	errno = 0;
	v = strtoull(s, &end, 10);
	if (errno || *end)
		return -1;
	*out = v;
	return 0;
}

// 128 (pages) or 25% / 12.5%
static int parse_density(const char *s, uint32_t *out)
{
	size_t n = strlen(s);
	uint64_t v;

	if (n > 1 && s[n - 1] == '%') {
		char tmp[32], *end;
		double pct, pages;
		uint32_t k;

		if (n - 1 >= sizeof(tmp))
			return -1;
		memcpy(tmp, s, n - 1);
		tmp[n - 1] = '\0';
		pct = strtod(tmp, &end);
		if (*end || !(pct > 0.0) || pct > 100.0)
			return -1;
		pages = pct * PROMO_REGION_PAGES / 100.0;
		k = (uint32_t)pages;
		if (pages - (double)k > 1e-9)
			k++;
		*out = k < 1 ? 1 : k;
		return 0;
	}
	if (parse_u64(s, &v) || v < 1 || v > PROMO_REGION_PAGES)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

int promo_config_parse(const char *spec, promo_config *out, char *err, size_t errlen)
{
	promo_config c;
	char *buf, *save = NULL, *tok;
	char vmsg[160];

	promo_config_default(&c);
	buf = strdup(spec ? spec : "");
	if (!buf) {
		if (err && errlen)
			snprintf(err, errlen, "out of memory");
		return -ENOMEM;
	}

	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		char *eq, *key, *val;
		uint64_t v;

		tok = trim(tok);
		if (!*tok)
			continue;
		eq = strchr(tok, '=');
		if (!eq)
			PARSE_FAIL("expected key=value, got '%s'", tok);
		*eq = '\0';
		key = trim(tok);
		val = trim(eq + 1);

		if (!strcmp(key, "kind")) {
			if (!strcmp(val, "never"))          c.kind = PROMO_POLICY_NEVER;
			else if (!strcmp(val, "always"))    c.kind = PROMO_POLICY_ALWAYS;
			else if (!strcmp(val, "threshold")) c.kind = PROMO_POLICY_THRESHOLD;
			else PARSE_FAIL("kind must be never|always|threshold, got '%s'", val);
		} else if (!strcmp(key, "combine")) {
			if (!strcmp(val, "density"))   c.combine = PROMO_COMBINE_DENSITY;
			else if (!strcmp(val, "run"))  c.combine = PROMO_COMBINE_RUN;
			else if (!strcmp(val, "and"))  c.combine = PROMO_COMBINE_AND;
			else if (!strcmp(val, "or"))   c.combine = PROMO_COMBINE_OR;
			else PARSE_FAIL("combine must be density|run|and|or, got '%s'", val);
		} else if (!strcmp(key, "density")) {
			if (parse_density(val, &c.min_touched))
				PARSE_FAIL("density must be 1..512 pages or a percent in (0,100], got '%s'", val);
		} else if (!strcmp(key, "run")) {
			if (parse_u64(val, &v) || v < 1 || v > PROMO_REGION_PAGES)
				PARSE_FAIL("run must be 1..512 pages, got '%s'", val);
			c.min_run = (uint32_t)v;
		} else if (!strcmp(key, "budget")) {
			if (parse_u64(val, &v) || v > UINT32_MAX)
				PARSE_FAIL("budget must be a non-negative integer, got '%s'", val);
			c.max_promoted = (uint32_t)v;
		} else if (!strcmp(key, "attempts")) {
			if (parse_u64(val, &v) || v > 1000)
				PARSE_FAIL("attempts must be 1..1000, got '%s'", val);
			c.max_attempts = (uint32_t)v;
		} else if (!strcmp(key, "backoff_ms")) {
			if (parse_u64(val, &v) || v > 3600000ull)
				PARSE_FAIL("backoff_ms must be 0..3600000, got '%s'", val);
			c.backoff_ns = v * 1000000ull;
		} else if (!strcmp(key, "backoff_max_ms")) {
			if (parse_u64(val, &v) || v > 3600000ull)
				PARSE_FAIL("backoff_max_ms must be 0..3600000, got '%s'", val);
			c.backoff_max_ns = v * 1000000ull;
		} else if (!strcmp(key, "workers")) {
			if (parse_u64(val, &v) || v < 1 || v > 1024)
				PARSE_FAIL("workers must be 1..1024, got '%s'", val);
			c.workers = (uint32_t)v;
		} else {
			PARSE_FAIL("unknown key '%s'", key);
		}
	}
	free(buf);

	if (promo_config_validate(&c, vmsg, sizeof(vmsg))) {
		if (err && errlen)
			snprintf(err, errlen, "%s", vmsg);
		return -EINVAL;
	}
	*out = c;
	return 0;
}