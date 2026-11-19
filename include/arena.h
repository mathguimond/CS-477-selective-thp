#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

#define HUGA_PAGE_SIZE (2UL * 1024 * 1024)

typedef struct { void *addr; size_t size; } arena_t;

// Allocate memory region
int arena_create(arena_t *arena, size_t size);

// Release memory
void arena_destroy(arena_t *arena);

#endif
