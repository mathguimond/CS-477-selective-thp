#include "arena.h"

#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>

// Allocate memory arena
int arena_create(arena_t *arena, size_t size) {
	// Input validation
	if (arena == NULL) {
		errno = EINVAL;
		return -1;
	}

	arena->addr = NULL;
	arena->size = 0;

	if (size == 0 || size % HUGE_PAGE_SIZE != 0) {
		errno = EINVAL;
		return -1;
	}

	// Memory allocation
	size_t map_size = size + HUGE_PAGE_SIZE;

	void *mapping = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		return -1;
	}

	uintptr_t base = (uintptr_t)mapping;
	uintptr_t aligned = (base + HUGE_PAGE_SIZE - 1) & ~(uintptr_t)(HUGE_PAGE_SIZE-1);

	// Release unused memory
	size_t before = aligned - base;
	size_t after = map_size - before - size;

	if (before > 0 && munmap(mapping, before) == -1) {
		int saved_errno = errno;
		munmap(mapping, map_size);
		errno = saved_errno;
		return -1;
	}

	if (after > 0 && munmap((void *)(aligned + size), after) == -1) {
		int saved_errno = errno;
		munmap((void *)aligned, size+after);
		errno = saved_errno;
		return -1;
	}

	// Store arena
	arena->addr = (void *)aligned;
	arena->size = size;

	return 0;
}


// Release memory
void arena_destroy(arena_t *arena) {
	if (arena == NULL || arena->addr == NULL) {
		return;
	}

	if (munmap(arena->addr, arena->size) == 0) {
		arena->addr = NULL;
		arena->size = 0;
	}
}
