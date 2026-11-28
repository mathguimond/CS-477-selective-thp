#ifndef FGSHP_POLICY_H
#define FGSHP_POLICY_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	PROMO_PAGE_SIZE = 4096,
	PROMO_REGION_PAGES = 512,
	PROMO_REGION_SIZE = PROMO_PAGE_SIZE * PROMO_REGION_PAGES,
	PROMO_BITMAP_WORDS = PROMO_REGION_PAGES / 64
};

// ---- configuration 

typedef enum {
	PROMO_POLICY_NEVER = 0,  // never promote; stop tracking on first touch   
	PROMO_POLICY_ALWAYS,     // promote on first touch                        
	PROMO_POLICY_THRESHOLD   // promote when density and/or run test passes   
} promo_policy_kind;

typedef enum {
	PROMO_COMBINE_DENSITY = 0, // touched >= min_touched                      
	PROMO_COMBINE_RUN,         // longest contiguous run >= min_run           
	PROMO_COMBINE_AND,
	PROMO_COMBINE_OR
} promo_combine;

typedef struct promo_config {
	promo_policy_kind kind;
	promo_combine combine;
	uint32_t min_touched;     // 1..512 pages                                  
	uint32_t min_run;         // 1..512 contiguous pages                       
	uint32_t max_promoted;    // budget of promoted regions, 0 = unlimited     
	uint32_t max_attempts;    // collapse attempts per region, >= 1            
	uint64_t backoff_ns;      // delay after 1st transient failure, doubles    
	uint64_t backoff_max_ns;  // cap on the delay                              
	uint32_t workers;         // advisory: collapse worker threads             
} promo_config;

void promo_config_default(promo_config *cfg);
int  promo_config_validate(const promo_config *cfg, char *err, size_t errlen);

/*
 * Parse "key=value,key=value". Keys: kind (never|always|threshold),
 * density (pages "128" or percent "25%"), run (pages), combine
 * (density|run|and|or), budget, attempts, backoff_ms, backoff_max_ms,
 * workers. Unspecified keys keep promo_config_default() values.
 * Returns 0, or -EINVAL with a message in err.
 */
int promo_config_parse(const char *spec, promo_config *out, char *err, size_t errlen);

// ---- per-region state 

typedef enum {
	PROMO_STATE_TRACKING = 0, // registered with uffd, collecting touches      
	PROMO_STATE_PENDING,      // decided; waiting for a worker                 
	PROMO_STATE_COLLAPSING,   // worker is inside MADV_COLLAPSE                
	PROMO_STATE_RETRY_WAIT,   // transient failure; retry at retry_at_ns       
	PROMO_STATE_PROMOTED,
	PROMO_STATE_ABANDONED     // will stay 4 KiB (denied, failed or NEVER)     
} promo_state;

typedef struct promo_region {
	uint64_t base;                          // 2 MiB aligned address           
	uint64_t bitmap[PROMO_BITMAP_WORDS];    // first-touched 4 KiB pages       
	uint64_t first_touch_ns;
	uint64_t last_touch_ns;
	uint64_t retry_at_ns;
	uint64_t collapse_ns_total;
	uint32_t attempts;
	uint16_t touched;
	uint16_t max_run;
	promo_state state;
	int last_err;                           // errno of last collapse, 0 = ok  
} promo_region;

/* Returns -EINVAL if base is not 2 MiB aligned (the kernel silently does
 * nothing for such ranges, so we refuse them up front). */
int promo_region_init(promo_region *r, uint64_t base);
const char *promo_state_name(promo_state s);

// ---- policy 

typedef enum {
	PROMO_WAIT = 0,   // keep tracking                                         
	PROMO_NOW,        // unregister the range and queue the region             
	PROMO_NEVER,      // unregister the range, region stays 4 KiB              
	PROMO_INVALID     // bad page index                                        
} promo_decision;

typedef enum {
	PROMO_NEXT_DONE = 0,  // promoted                                          
	PROMO_NEXT_RETRY,     // requeue the region at region->retry_at_ns         
	PROMO_NEXT_GIVEUP,    // region stays 4 KiB                                
	PROMO_NEXT_INVALID    // region was not in a collapsible state             
} promo_next;

typedef struct promo_stats {
	uint64_t touches;
	uint64_t duplicate_touches;                        
	uint64_t late_touches;        // region no longer TRACKING     
	uint64_t promote_decisions;
	uint64_t never_decisions;
	uint64_t budget_denied;
	uint64_t est_zero_fill_bytes; // (512 - touched) * 4 KiB per decision      
	uint64_t collapse_ok;
	uint64_t collapse_eagain;
	uint64_t collapse_enomem;
	uint64_t collapse_ebusy;
	uint64_t collapse_einval;
	uint64_t collapse_other;
	uint64_t retries;
	uint64_t giveups;
	uint64_t collapse_ns_total;
	uint64_t collapse_ns_max;
} promo_stats;

typedef struct promo_policy {
	promo_config cfg;
	pthread_mutex_t lock;
	uint32_t reserved;            // regions holding budget                    
	promo_stats stats;
} promo_policy;

int  promo_policy_init(promo_policy *p, const promo_config *cfg);
void promo_policy_destroy(promo_policy *p);

// First touch of 4 KiB page page_idx (0..511) of region r. 
promo_decision promo_policy_touch(promo_policy *p, promo_region *r,
				  unsigned page_idx, uint64_t now_ns);

/* Worker: PENDING -> COLLAPSING, or RETRY_WAIT -> COLLAPSING once due.
 * 0 on success; -EAGAIN if a retry is not due yet; -EINVAL otherwise. */
int promo_policy_begin_collapse(promo_policy *p, promo_region *r, uint64_t now_ns);

/* Worker: report the result of madvise(MADV_COLLAPSE). err is 0 or the
 * positive errno. */
promo_next promo_policy_collapse_done(promo_policy *p, promo_region *r,
				      int err, uint64_t cost_ns, uint64_t now_ns);

promo_stats promo_policy_stats(promo_policy *p);

promo_next promo_run_collapse(promo_policy *p, promo_region *r);
uint64_t promo_now_ns(void);

#ifdef __cplusplus
}
#endif
#endif