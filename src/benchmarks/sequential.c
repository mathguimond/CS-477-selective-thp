#include "arena.h"

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

typedef struct {
	volatile char *memory;
	size_t start_region;
	size_t end_region;
	size_t page_to_touch;
	size_t hot_pages;
	size_t reps;
	size_t sum;
	pthread_barrier_t *barrier;
	const size_t *page_order;
} worker_args_t;

void *worker(void *arg) {
	worker_args_t *w = arg;

	pthread_barrier_wait(w->barrier);

	for (size_t region = w->start_region; region < w->end_region; region++) {
		for (size_t p = 0; p < w->page_to_touch; p++) {
			w->memory[region*HUGE_PAGE_SIZE + w->page_order[p]*4096] = 1;
		}
	}

	pthread_barrier_wait(w->barrier);
	pthread_barrier_wait(w->barrier);

	size_t sum = 0;
	for (size_t r=0; r< w->reps; r++) {
		for (size_t region = w->start_region; region < w->end_region; region++) {
			for (size_t p=0; p < w->hot_pages; p++) {
				sum += w->memory[region*HUGE_PAGE_SIZE + w->page_order[p]*4096];
			}
		}
	}

	w->sum = sum;
	pthread_barrier_wait(w->barrier);

	return NULL;
}

int main(int argc, char *argv[]) {
	// Get args
	size_t arena_size = (argc > 1) ? strtoul(argv[1], NULL, 10) : 64;
	size_t reps = (argc > 2) ? strtoul(argv[2], NULL, 10) : 100;
	size_t density = (argc > 3) ? strtoul(argv[3], NULL, 10) : 100;
	size_t nb_thread = (argc > 4) ? strtoul(argv[4], NULL, 10) : 1;
	int random_access = (argc > 5 && strcmp(argv[5], "random") == 0);
	size_t hot_density = (argc > 6) ? strtoul(argv[6], NULL, 10) : density;

	if (arena_size == 0 || arena_size%2 != 0 || reps == 0 || density > 100 || nb_thread == 0 || nb_thread > arena_size/2 || hot_density > density) {
		fprintf(stderr, "Invalid args\n");
		return 1;
	}

	// Allocate memory
	arena_t arena;
	if (arena_create(&arena, arena_size*1024*1024) == -1) {
		perror("arena_create");
		return 1;
	}

	size_t nb_region = arena.size / HUGE_PAGE_SIZE;
	size_t page_to_touch = (HUGE_PAGE_SIZE/4096) * density / 100;
	size_t hot_pages = (HUGE_PAGE_SIZE/4096) * hot_density / 100;

	size_t page_order[HUGE_PAGE_SIZE/4096];
	for (size_t i=0; i<HUGE_PAGE_SIZE/4096; i++) {
		page_order[i] = i;
	}

	if (random_access) {
		srand(42);

		for (size_t i = HUGE_PAGE_SIZE/4096 -1; i>0; i--) {
			size_t j = rand() % (i+1);
			size_t temp = page_order[i];
			page_order[i] = page_order[j];
			page_order[j] = temp;
		}
	}

	pthread_t threads[nb_thread];
	worker_args_t args[nb_thread];
	pthread_barrier_t barrier;

	if (pthread_barrier_init(&barrier, NULL, nb_thread+1) != 0) {
		fprintf(stderr, "Failed to initialize barrier\n");
		arena_destroy(&arena);
		return 1;
	}

	// Create threads
	for (size_t i=0; i<nb_thread; i++) {
		args[i] = (worker_args_t) {
			.memory = arena.addr,
			.start_region = i * nb_region / nb_thread,
			.end_region = (i+1) * nb_region / nb_thread,
			.page_to_touch = page_to_touch,
			.hot_pages = hot_pages,
			.reps = reps,
			.sum = 0,
			.barrier = &barrier,
			.page_order = page_order
		};

		if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
			fprintf(stderr, "Failed to create thread\n");
			exit(1);
		}
	}

	struct timespec cold_start, cold_end, hot_start, hot_end;

	// Measure cold time
	clock_gettime(CLOCK_MONOTONIC, &cold_start);
	pthread_barrier_wait(&barrier);
	pthread_barrier_wait(&barrier);
	clock_gettime(CLOCK_MONOTONIC, &cold_end);

	// Measure hot time
	clock_gettime(CLOCK_MONOTONIC, &hot_start);
	pthread_barrier_wait(&barrier);
	pthread_barrier_wait(&barrier);
	clock_gettime(CLOCK_MONOTONIC, &hot_end);

	size_t sum = 0;
	for (size_t i=0; i<nb_thread; i++) {
		pthread_join(threads[i], NULL);
		sum += args[i].sum;
	}

	double cold = (cold_end.tv_sec - cold_start.tv_sec)*1000.0 + (cold_end.tv_nsec - cold_start.tv_nsec)/1000000.0;
	double hot = (hot_end.tv_sec - hot_start.tv_sec)*1000.0 + (hot_end.tv_nsec - hot_start.tv_nsec)/1000000.0;

	printf("Cold time: %.3f ms\n", cold);
	printf("Hot time / reps: %.3f ms\n", hot/reps);
	printf("Sum: %zu\n", sum);

	pthread_barrier_destroy(&barrier);
	arena_destroy(&arena);

	return 0;
}
