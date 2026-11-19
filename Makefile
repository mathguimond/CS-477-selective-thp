CC = gcc

CPPFLAGS = -Iinclude -D_GNU_SOURCE
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic

DEBUG_FLAGS = -O0 -g3
RELEASE_FLAGS = -O2 -g

.PHONY: all check clean

all: check

check:
	@printf '#include "arena.h"\n#include "benchmark.h"\nint main(void) { return 0; }\n' | $(CC) $(CPPFLAGS) $(CFLAGS) -x c -fsyntax-only -
	@echo "Shared headers compile sucessfully."

clean:
	rm -rf build bin
