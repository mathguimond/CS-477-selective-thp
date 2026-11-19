#ifndef BENCHMARK_H
#define BENCHMARK_H

#include <stddef.h>
#include <stdint.h>

// Benchmark configuration
typedef struct {
	size_t arena_size;		// Memory size
	unsigned int threads;		// Nb of threads
	unsigned int repetitions;	// Nb of workload repetition
	uint64_t seed;			// Seed for random access
} benchmark_config_t;

// Bechmark results
typedef struct {
	double cold_seconds;		// Initial memomry access time
	double hot_seconds;		// Subsequent access time
	uint64_t checksum;		// Check correctness
} benchmark_result_t;

#endif
