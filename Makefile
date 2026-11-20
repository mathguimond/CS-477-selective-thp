CC = gcc

CPPFLAGS = -Iinclude -D_GNU_SOURCE
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic

DEBUG_FLAGS = -O0 -g3
RELEASE_FLAGS = -O2 -g

.PHONY: all check test clean

all: check

check:
	@printf '#include "arena.h"\n#include "benchmark.h"\nint main(void) { return 0; }\n' | $(CC) $(CPPFLAGS) $(CFLAGS) -x c -fsyntax-only -
	@echo "Shared headers compile sucessfully."


TEST_SRCS = $(wildcard tests/test_*.c)
TEST_BINS = $(patsubst tests/%.c,bin/%,$(TEST_SRCS))
COMMON_SRCS = $(wildcard src/common/*.c)

bin/%: tests/%.c $(COMMON_SRCS) $(wildcard include/*.h)
	@mkdir -p bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(COMMON_SRCS) -o $@

test: $(TEST_BINS)
	@set -e; for test in $(TEST_BINS); do \
		echo "Running $$test"; \
		./$$test; \
	done


clean:
	rm -rf build bin
