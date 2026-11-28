#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
 
#include "policy.h"
 
#include <errno.h>
#include <sys/mman.h>
#include <time.h>
 
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

uint64_t promo_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

promo_next promo_run_collapse(promo_policy *p, promo_region *r)
{
	uint64_t t0 = promo_now_ns(), t1;
	int err = 0;

	if (promo_policy_begin_collapse(p, r, t0) != 0)
		return PROMO_NEXT_INVALID;

	if (madvise((void *)(uintptr_t)r->base, PROMO_REGION_SIZE, MADV_COLLAPSE) != 0)
		err = errno;

	t1 = promo_now_ns();
	return promo_policy_collapse_done(p, r, err, t1 - t0, t1);
}