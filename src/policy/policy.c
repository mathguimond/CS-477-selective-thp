#include "policy.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define CFG_FAIL(...)                                                   \
	do {                                                            \
		if (err && errlen)                                      \
			snprintf(err, errlen, __VA_ARGS__);             \
		return -EINVAL;                                         \
	} while (0)

#define BACKOFF_LIMIT_NS (3600ull * 1000000000ull)

void promo_config_default(promo_config *c)
{
	memset(c, 0, sizeof(*c));
	c->kind = PROMO_POLICY_THRESHOLD;
	c->combine = PROMO_COMBINE_DENSITY;
	c->min_touched = 256;
	c->min_run = 64;
	c->max_promoted = 0;
	c->max_attempts = 3;
	c->backoff_ns = 5ull * 1000000ull;
	c->backoff_max_ns = 200ull * 1000000ull;
	c->workers = 1;
}

int promo_config_validate(const promo_config *c, char *err, size_t errlen)
{
	if ((int)c->kind < 0 || c->kind > PROMO_POLICY_THRESHOLD)
		CFG_FAIL("unknown policy kind %d", (int)c->kind);
	if ((int)c->combine < 0 || c->combine > PROMO_COMBINE_OR)
		CFG_FAIL("unknown combine mode %d", (int)c->combine);
	if (c->kind == PROMO_POLICY_THRESHOLD) {
		bool uses_density = c->combine != PROMO_COMBINE_RUN;
		bool uses_run = c->combine != PROMO_COMBINE_DENSITY;

		if (uses_density &&
		    (c->min_touched < 1 || c->min_touched > PROMO_REGION_PAGES))
			CFG_FAIL("density must be 1..%d pages (got %u)",
				 PROMO_REGION_PAGES, c->min_touched);
		if (uses_run &&
		    (c->min_run < 1 || c->min_run > PROMO_REGION_PAGES))
			CFG_FAIL("run must be 1..%d pages (got %u)",
				 PROMO_REGION_PAGES, c->min_run);
	}
	if (c->max_attempts < 1)
		CFG_FAIL("attempts must be >= 1");
	if (c->backoff_max_ns < c->backoff_ns)
		CFG_FAIL("backoff_max_ms must be >= backoff_ms");
	if (c->backoff_max_ns > BACKOFF_LIMIT_NS)
		CFG_FAIL("backoff_max_ms must be <= 3600000");
	if (c->workers < 1)
		CFG_FAIL("workers must be >= 1");
	return 0;
}

// ---- bitmap helpers

static inline bool bit_get(const uint64_t *bm, unsigned i)
{
	return (bm[i >> 6] >> (i & 63)) & 1u;
}

static inline void bit_set(uint64_t *bm, unsigned i)
{
	bm[i >> 6] |= 1ull << (i & 63);
}

// Length of the run of set bits that contains bit i.
static unsigned run_through(const uint64_t *bm, unsigned i)
{
	unsigned lo = i, hi = i;

	while (lo > 0 && bit_get(bm, lo - 1))
		lo--;
	while (hi + 1 < PROMO_REGION_PAGES && bit_get(bm, hi + 1))
		hi++;
	return hi - lo + 1;
}

// ---- regions 

int promo_region_init(promo_region *r, uint64_t base)
{
	memset(r, 0, sizeof(*r));
	if (base & (PROMO_REGION_SIZE - 1))
		return -EINVAL;
	r->base = base;
	r->state = PROMO_STATE_TRACKING;
	return 0;
}

const char *promo_state_name(promo_state s)
{
	switch (s) {
	case PROMO_STATE_TRACKING:   return "tracking";
	case PROMO_STATE_PENDING:    return "pending";
	case PROMO_STATE_COLLAPSING: return "collapsing";
	case PROMO_STATE_RETRY_WAIT: return "retry_wait";
	case PROMO_STATE_PROMOTED:   return "promoted";
	case PROMO_STATE_ABANDONED:  return "abandoned";
	}
	return "?";
}

// ---- policy

int promo_policy_init(promo_policy *p, const promo_config *cfg)
{
	int rc = promo_config_validate(cfg, NULL, 0);

	if (rc)
		return rc;
	memset(p, 0, sizeof(*p));
	p->cfg = *cfg;
	return pthread_mutex_init(&p->lock, NULL) ? -ENOMEM : 0;
}

void promo_policy_destroy(promo_policy *p)
{
	pthread_mutex_destroy(&p->lock);
}

static bool meets(const promo_config *c, unsigned touched, unsigned run)
{
	bool d = touched >= c->min_touched;
	bool r = run >= c->min_run;

	switch (c->combine) {
	case PROMO_COMBINE_DENSITY: return d;
	case PROMO_COMBINE_RUN:     return r;
	case PROMO_COMBINE_AND:     return d && r;
	case PROMO_COMBINE_OR:      return d || r;
	}
	return false;
}

// Caller holds the lock.
static promo_decision start_promotion(promo_policy *p, promo_region *r)
{
	if (p->cfg.max_promoted && p->reserved >= p->cfg.max_promoted) {
		p->stats.budget_denied++;
		p->stats.never_decisions++;
		r->state = PROMO_STATE_ABANDONED;
		return PROMO_NEVER;
	}
	p->reserved++;
	p->stats.promote_decisions++;
	p->stats.est_zero_fill_bytes +=
		(uint64_t)(PROMO_REGION_PAGES - r->touched) * PROMO_PAGE_SIZE;
	r->state = PROMO_STATE_PENDING;
	return PROMO_NOW;
}

promo_decision promo_policy_touch(promo_policy *p, promo_region *r,
				  unsigned page_idx, uint64_t now_ns)
{
	promo_decision d = PROMO_WAIT;
	unsigned run;

	if (page_idx >= PROMO_REGION_PAGES)
		return PROMO_INVALID;

	pthread_mutex_lock(&p->lock);
	p->stats.touches++;

	if (r->state != PROMO_STATE_TRACKING) {
		// Fault read before the range was unregistered. 
		p->stats.late_touches++;
		goto out;
	}
	if (bit_get(r->bitmap, page_idx)) {
		p->stats.duplicate_touches++;
		goto out;
	}

	bit_set(r->bitmap, page_idx);
	r->touched++;
	if (r->touched == 1)
		r->first_touch_ns = now_ns;
	r->last_touch_ns = now_ns;
	run = run_through(r->bitmap, page_idx);
	if (run > r->max_run)
		r->max_run = (uint16_t)run;

	switch (p->cfg.kind) {
	case PROMO_POLICY_NEVER:
		p->stats.never_decisions++;
		r->state = PROMO_STATE_ABANDONED;
		d = PROMO_NEVER;
		break;
	case PROMO_POLICY_ALWAYS:
		d = start_promotion(p, r);
		break;
	case PROMO_POLICY_THRESHOLD:
		if (meets(&p->cfg, r->touched, r->max_run))
			d = start_promotion(p, r);
		break;
	}
out:
	pthread_mutex_unlock(&p->lock);
	return d;
}

int promo_policy_begin_collapse(promo_policy *p, promo_region *r, uint64_t now_ns)
{
	int rc = 0;

	pthread_mutex_lock(&p->lock);
	if (r->state == PROMO_STATE_PENDING) {
		r->state = PROMO_STATE_COLLAPSING;
	} else if (r->state == PROMO_STATE_RETRY_WAIT) {
		if (now_ns < r->retry_at_ns)
			rc = -EAGAIN;
		else
			r->state = PROMO_STATE_COLLAPSING;
	} else {
		rc = -EINVAL;
	}
	pthread_mutex_unlock(&p->lock);
	return rc;
}

static uint64_t backoff_for(const promo_config *c, uint32_t attempts)
{
	uint64_t d = c->backoff_ns;

	for (uint32_t i = 1; i < attempts && d < c->backoff_max_ns; i++)
		d *= 2;
	return d > c->backoff_max_ns ? c->backoff_max_ns : d;
}

promo_next promo_policy_collapse_done(promo_policy *p, promo_region *r,
				      int err, uint64_t cost_ns, uint64_t now_ns)
{
	promo_next next;
	bool transient;

	pthread_mutex_lock(&p->lock);
	if (r->state != PROMO_STATE_COLLAPSING) {
		pthread_mutex_unlock(&p->lock);
		return PROMO_NEXT_INVALID;
	}

	r->attempts++;
	r->last_err = err;
	r->collapse_ns_total += cost_ns;
	p->stats.collapse_ns_total += cost_ns;
	if (cost_ns > p->stats.collapse_ns_max)
		p->stats.collapse_ns_max = cost_ns;

	if (err == 0) {
		p->stats.collapse_ok++;
		r->state = PROMO_STATE_PROMOTED;
		next = PROMO_NEXT_DONE;
		goto out;
	}

	switch (err) {
	case EAGAIN: p->stats.collapse_eagain++; break;
	case ENOMEM: p->stats.collapse_enomem++; break;
	case EBUSY:  p->stats.collapse_ebusy++;  break;
	case EINVAL: p->stats.collapse_einval++; break;
	default:     p->stats.collapse_other++;  break;
	}

	transient = err == EAGAIN || err == ENOMEM || err == EBUSY;
	if (transient && r->attempts < p->cfg.max_attempts) {
		r->retry_at_ns = now_ns + backoff_for(&p->cfg, r->attempts);
		r->state = PROMO_STATE_RETRY_WAIT;
		p->stats.retries++;
		next = PROMO_NEXT_RETRY;
	} else {
		r->state = PROMO_STATE_ABANDONED;
		p->reserved--;
		p->stats.giveups++;
		next = PROMO_NEXT_GIVEUP;
	}
out:
	pthread_mutex_unlock(&p->lock);
	return next;
}

promo_stats promo_policy_stats(promo_policy *p)
{
	promo_stats s;

	pthread_mutex_lock(&p->lock);
	s = p->stats;
	pthread_mutex_unlock(&p->lock);
	return s;
}