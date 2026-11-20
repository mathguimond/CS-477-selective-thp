#include "arena.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
	arena_t arena;
	size_t size = 4*HUGE_PAGE_SIZE;

	// Test allocation
	assert(arena_create(&arena, size) == 0);

	assert(arena.size == size);
	assert((uintptr_t)arena.addr % HUGE_PAGE_SIZE == 0);

	// Test read/write
	char *memory = arena.addr;
	for (size_t i=0; i<size; i+=4096) {
		memory[i] = 42;
		assert(memory[i] == 42);
	}

	// Test cleanup
	arena_destroy(&arena);
	assert(arena.addr == NULL);
	assert(arena.size == 0);

	printf("All tests passed!\n");
	return 0;
}
