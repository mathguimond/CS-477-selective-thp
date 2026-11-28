#define _POSIX_C_SOURCE 200809L

#include "policy.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(c)                                                              \
	do {                                                                  \
		if (!(c)) {                                                   \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, \
				__LINE__, #c);                                \
			failures++;                                           \
		}                                                             \
	} while (0)

#define MS 1000000ull

static void mk(promo_policy *p, const char *spec)
{
	promo_config c;
	char err[160];

	if (promo_config_parse(spec, &c, err, sizeof(err))) {
		fprintf(stderr, "bad spec '%s': %s\n", spec, err);
		exit(2);
	}
	if (promo_policy_init(p, &c))
		exit(2);
}

static void mkregion(promo_region *r, uint64_t i)
{
	if (promo_region_init(r, 0x40000000ull + i * PROMO_REGION_SIZE))
		exit(2);
}

// run / density tracking

static void test_run_tracking(void)
{
	promo_policy p;
	promo_region r;

	mk(&p, "density=512");
	mkregion(&r, 0);

	promo_policy_touch(&p, &r, 63, 1);
	promo_policy_touch(&p, &r, 65, 1);
	CHECK(r.max_run == 1);
	promo_policy_touch(&p, &r, 64, 1);      // joins across a word boundary 
	CHECK(r.max_run == 3);
	promo_policy_touch(&p, &r, 0, 1);
	promo_policy_touch(&p, &r, 1, 1);
	CHECK(r.max_run == 3);
	for (unsigned i = 2; i <= 5; i++)
		promo_policy_touch(&p, &r, i, 1);
	CHECK(r.max_run == 6);
	for (unsigned i = 62; i >= 59; i--)
		promo_policy_touch(&p, &r, i, 1);
	CHECK(r.max_run == 7);                  // 59..65 
	promo_policy_touch(&p, &r, 511, 1);
	promo_policy_touch(&p, &r, 510, 1);
	CHECK(r.max_run == 7);
	CHECK(r.touched == 15);
	CHECK(r.state == PROMO_STATE_TRACKING);
	promo_policy_destroy(&p);
}

static void test_density_edge(void)
{
	promo_policy p;
	promo_region r;

	mk(&p, "density=25%");                  // 128 pages 
	mkregion(&r, 0);
	for (unsigned i = 0; i < 127; i++)
		CHECK(promo_policy_touch(&p, &r, i * 2, 1) == PROMO_WAIT);
	CHECK(promo_policy_touch(&p, &r, 254, 1) == PROMO_NOW);
	CHECK(r.state == PROMO_STATE_PENDING);
	CHECK(promo_policy_stats(&p).est_zero_fill_bytes == 384ull * PROMO_PAGE_SIZE);
	promo_policy_destroy(&p);
}

static void test_run_threshold(void)
{
	promo_policy p;
	promo_region r;

	mk(&p, "combine=run,run=16");
	mkregion(&r, 0);
	for (unsigned i = 0; i < 15; i++)
		CHECK(promo_policy_touch(&p, &r, 100 + i, 1) == PROMO_WAIT);
	for (unsigned i = 0; i < 100; i += 2)   // scattered pages do not help 
		CHECK(promo_policy_touch(&p, &r, i, 1) == PROMO_WAIT);
	CHECK(promo_policy_touch(&p, &r, 115, 1) == PROMO_NOW);
	promo_policy_destroy(&p);
}

static void test_combine_modes(void)
{
	promo_policy p;
	promo_region r;

	mk(&p, "combine=and,density=8,run=4");
	mkregion(&r, 0);
	for (unsigned i = 0; i < 16; i += 2)    // 8 pages, runs of 1 
		CHECK(promo_policy_touch(&p, &r, i, 1) == PROMO_WAIT);
	CHECK(promo_policy_touch(&p, &r, 1, 1) == PROMO_WAIT); // run 0..2 = 3 
	CHECK(promo_policy_touch(&p, &r, 3, 1) == PROMO_NOW);  // run 0..4 = 5 
	promo_policy_destroy(&p);

	mk(&p, "combine=or,density=100,run=4");
	mkregion(&r, 1);
	for (unsigned i = 0; i < 3; i++)
		CHECK(promo_policy_touch(&p, &r, i, 1) == PROMO_WAIT);
	CHECK(promo_policy_touch(&p, &r, 3, 1) == PROMO_NOW);
	promo_policy_destroy(&p);
}

static void test_always_never(void)
{
	promo_policy p;
	promo_region r;

	mk(&p, "kind=always");
	mkregion(&r, 0);
	CHECK(promo_policy_touch(&p, &r, 300, 1) == PROMO_NOW);
	promo_policy_destroy(&p);

	mk(&p, "kind=never");
	mkregion(&r, 0);
	CHECK(promo_policy_touch(&p, &r, 300, 1) == PROMO_NEVER);
	CHECK(r.state == PROMO_STATE_ABANDONED);
	CHECK(promo_policy_stats(&p).never_decisions == 1);
	promo_policy_destroy(&p);
}

// ---- budget and collapse results 

static void test_budget(void)
{
	promo_policy p;
	promo_region r[4];

	mk(&p, "kind=always,budget=2,attempts=1");
	for (unsigned i = 0; i < 4; i++)
		mkregion(&r[i], i);
	CHECK(promo_policy_touch(&p, &r[0], 0, 1) == PROMO_NOW);
	CHECK(promo_policy_touch(&p, &r[1], 0, 1) == PROMO_NOW);
	CHECK(promo_policy_touch(&p, &r[2], 0, 1) == PROMO_NEVER);
	CHECK(r[2].state == PROMO_STATE_ABANDONED);
	CHECK(promo_policy_stats(&p).budget_denied == 1);

	// A failed region gives its budget back. 
	CHECK(promo_policy_begin_collapse(&p, &r[0], 10) == 0);
	CHECK(promo_policy_collapse_done(&p, &r[0], EINVAL, 5, 10) == PROMO_NEXT_GIVEUP);
	CHECK(promo_policy_touch(&p, &r[3], 0, 20) == PROMO_NOW);
	promo_policy_destroy(&p);
}

static void test_collapse_flow(void)
{
	promo_policy p;
	promo_region r;

	mk(&p, "kind=always,attempts=5,backoff_ms=10,backoff_max_ms=25");
	mkregion(&r, 0);
	CHECK(promo_policy_touch(&p, &r, 0, 0) == PROMO_NOW);
	CHECK(promo_policy_begin_collapse(&p, &r, 0) == 0);
	CHECK(r.state == PROMO_STATE_COLLAPSING);
	CHECK(promo_policy_begin_collapse(&p, &r, 0) == -EINVAL);

	CHECK(promo_policy_collapse_done(&p, &r, EAGAIN, 100, 1000) == PROMO_NEXT_RETRY);
	CHECK(r.state == PROMO_STATE_RETRY_WAIT);
	CHECK(r.retry_at_ns == 1000 + 10 * MS);
	CHECK(promo_policy_begin_collapse(&p, &r, 1000 + 10 * MS - 1) == -EAGAIN);
	CHECK(promo_policy_begin_collapse(&p, &r, 1000 + 10 * MS) == 0);

	CHECK(promo_policy_collapse_done(&p, &r, ENOMEM, 100, 0) == PROMO_NEXT_RETRY);
	CHECK(r.retry_at_ns == 20 * MS);
	CHECK(promo_policy_begin_collapse(&p, &r, 20 * MS) == 0);
	CHECK(promo_policy_collapse_done(&p, &r, EBUSY, 100, 0) == PROMO_NEXT_RETRY);
	CHECK(r.retry_at_ns == 25 * MS);        // 40 ms capped to 25 ms 
	CHECK(promo_policy_begin_collapse(&p, &r, 25 * MS) == 0);
	CHECK(promo_policy_collapse_done(&p, &r, 0, 700, 0) == PROMO_NEXT_DONE);
	CHECK(r.state == PROMO_STATE_PROMOTED);
	CHECK(r.attempts == 4);
	CHECK(r.collapse_ns_total == 1000);
	CHECK(promo_policy_begin_collapse(&p, &r, 0) == -EINVAL);
	CHECK(promo_policy_collapse_done(&p, &r, 0, 1, 0) == PROMO_NEXT_INVALID);

	promo_policy_destroy(&p);

	// Attempts exhausted, and permanent errors give up immediately. 
	mk(&p, "kind=always,attempts=2,backoff_ms=1,backoff_max_ms=1");
	mkregion(&r, 0);
	promo_policy_touch(&p, &r, 0, 0);
	promo_policy_begin_collapse(&p, &r, 0);
	CHECK(promo_policy_collapse_done(&p, &r, ENOMEM, 1, 0) == PROMO_NEXT_RETRY);
	promo_policy_begin_collapse(&p, &r, 1 * MS);
	CHECK(promo_policy_collapse_done(&p, &r, ENOMEM, 1, 0) == PROMO_NEXT_GIVEUP);
	CHECK(r.state == PROMO_STATE_ABANDONED);

	mkregion(&r, 1);
	promo_policy_touch(&p, &r, 0, 0);
	promo_policy_begin_collapse(&p, &r, 0);
	CHECK(promo_policy_collapse_done(&p, &r, EINVAL, 1, 0) == PROMO_NEXT_GIVEUP);
	CHECK(r.attempts == 1);

	{
		promo_stats s = promo_policy_stats(&p);
		CHECK(s.collapse_enomem == 2);
		CHECK(s.collapse_einval == 1);
		CHECK(s.retries == 1);
		CHECK(s.giveups == 2);
	}
	promo_policy_destroy(&p);
}

static void test_late_duplicate_invalid(void)
{
	promo_policy p;
	promo_region r;
	promo_stats s;

	mk(&p, "density=2");
	mkregion(&r, 0);
	CHECK(promo_policy_touch(&p, &r, 5, 1) == PROMO_WAIT);
	CHECK(promo_policy_touch(&p, &r, 5, 2) == PROMO_WAIT);   // duplicate 
	CHECK(r.touched == 1);
	CHECK(promo_policy_touch(&p, &r, 6, 3) == PROMO_NOW);
	CHECK(promo_policy_touch(&p, &r, 7, 4) == PROMO_WAIT);   // late 
	CHECK(r.touched == 2);
	CHECK(promo_policy_touch(&p, &r, 512, 5) == PROMO_INVALID);
	s = promo_policy_stats(&p);
	CHECK(s.duplicate_touches == 1);
	CHECK(s.late_touches == 1);
	CHECK(r.first_touch_ns == 1 && r.last_touch_ns == 3);
	promo_policy_destroy(&p);

	CHECK(promo_region_init(&r, 0x40000000ull + 4096) == -EINVAL); // misaligned 
}

// ---- property test: decision depends on the set, not the order -------- 

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

static void shuffle(unsigned *a, unsigned n)
{
	for (unsigned i = n; i > 1; i--) {
		unsigned j = (unsigned)(rng() % i), t = a[i - 1];

		a[i - 1] = a[j];
		a[j] = t;
	}
}

static unsigned brute_max_run(const bool *set)
{
	unsigned best = 0, cur = 0;

	for (unsigned i = 0; i < PROMO_REGION_PAGES; i++) {
		cur = set[i] ? cur + 1 : 0;
		if (cur > best)
			best = cur;
	}
	return best;
}

static void test_property(void)
{
	promo_policy p;
	unsigned idx[PROMO_REGION_PAGES];

	mk(&p, "combine=or,density=40,run=10");
	for (int iter = 0; iter < 300; iter++) {
		unsigned k = (unsigned)(rng() % 120);
		bool set[PROMO_REGION_PAGES] = { false };
		promo_state first_state = PROMO_STATE_TRACKING;

		for (unsigned i = 0; i < PROMO_REGION_PAGES; i++)
			idx[i] = i;
		shuffle(idx, PROMO_REGION_PAGES);
		for (unsigned i = 0; i < k; i++)
			set[idx[i]] = true;
		bool expect = k >= 40 || brute_max_run(set) >= 10;

		for (int order = 0; order < 2; order++) {
			promo_region r;

			mkregion(&r, (uint64_t)iter);
			shuffle(idx, k);
			for (unsigned i = 0; i < k; i++)
				promo_policy_touch(&p, &r, idx[i], 1);
			CHECK((r.state == PROMO_STATE_PENDING) == expect);
			if (order == 0)
				first_state = r.state;
			else
				CHECK(r.state == first_state);
			if (r.state == PROMO_STATE_TRACKING) {
				CHECK(r.touched == k);
				CHECK(r.max_run == brute_max_run(set));
			}
		}
	}
	promo_policy_destroy(&p);
}

// ---- race test: exactly one thread wins the promotion 

struct targ {
	promo_policy *p;
	promo_region *r;
	unsigned first;
	atomic_int *nows;
};

static void *thr(void *a)
{
	struct targ *t = a;

	for (unsigned i = 0; i < 64; i++)
		if (promo_policy_touch(t->p, t->r, t->first + i, 1) == PROMO_NOW)
			atomic_fetch_add(t->nows, 1);
	return NULL;
}

static void test_thread_race(void)
{
	for (int round = 0; round < 50; round++) {
		promo_policy p;
		promo_region r;
		pthread_t th[8];
		struct targ ta[8];
		atomic_int nows = 0;
		promo_stats s;

		mk(&p, "density=256");
		mkregion(&r, 0);
		for (unsigned t = 0; t < 8; t++) {
			ta[t] = (struct targ){ &p, &r, t * 64, &nows };
			pthread_create(&th[t], NULL, thr, &ta[t]);
		}
		for (unsigned t = 0; t < 8; t++)
			pthread_join(th[t], NULL);

		s = promo_policy_stats(&p);
		CHECK(atomic_load(&nows) == 1);
		CHECK(r.touched == 256);
		CHECK(s.touches == 512);
		CHECK(s.late_touches == 256);
		CHECK(r.state == PROMO_STATE_PENDING);
		promo_policy_destroy(&p);
	}
}

// ---- config parsing 

static void test_parse(void)
{
	promo_config c;
	char err[160];

	CHECK(promo_config_parse("", &c, err, sizeof(err)) == 0);
	CHECK(c.kind == PROMO_POLICY_THRESHOLD && c.min_touched == 256);

	CHECK(promo_config_parse(" kind=threshold, density=25%, run=64, combine=or,"
				 " budget=100, attempts=4, backoff_ms=2,"
				 " backoff_max_ms=50, workers=2",
				 &c, err, sizeof(err)) == 0);
	CHECK(c.min_touched == 128 && c.min_run == 64 && c.combine == PROMO_COMBINE_OR);
	CHECK(c.max_promoted == 100 && c.max_attempts == 4 && c.workers == 2);
	CHECK(c.backoff_ns == 2 * MS && c.backoff_max_ns == 50 * MS);

	CHECK(promo_config_parse("density=12.5%", &c, err, sizeof(err)) == 0 && c.min_touched == 64);
	CHECK(promo_config_parse("density=0.1%", &c, err, sizeof(err)) == 0 && c.min_touched == 1);
	CHECK(promo_config_parse("density=100%", &c, err, sizeof(err)) == 0 && c.min_touched == 512);
	CHECK(promo_config_parse("kind=always,density=999", &c, err, sizeof(err)) != 0);

	static const char *const bad[] = {
		"bogus=1", "density=0", "density=513", "density=abc", "density=0%",
		"density=101%", "combine=xor", "kind=sometimes", "density", "attempts=0",
		"run=0", "workers=0", "backoff_ms=10,backoff_max_ms=5", "budget=-1",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		err[0] = '\0';
		if (promo_config_parse(bad[i], &c, err, sizeof(err)) == 0 || !err[0]) {
			fprintf(stderr, "spec '%s' should fail with a message\n", bad[i]);
			failures++;
		}
	}
}

int main(void)
{
	test_run_tracking();
	test_density_edge();
	test_run_threshold();
	test_combine_modes();
	test_always_never();
	test_budget();
	test_collapse_flow();
	test_late_duplicate_invalid();
	test_property();
	test_thread_race();
	test_parse();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all policy tests passed");
	return 0;
}