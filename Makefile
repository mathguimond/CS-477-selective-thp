CC = gcc

CPPFLAGS = -Iinclude -D_GNU_SOURCE
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic

DEBUG_FLAGS = -O0 -g3
RELEASE_FLAGS = -O2 -g

LINUX_SRC ?=

# Sanitizer builds
ASAN_FLAGS = -O1 -g -fsanitize=adress,undefined -fno-sanitize-recover=all
TSAN_FLAGS = -O1 -g -fsanitize=thread

.PHONY: all check test test-asan test-tsan test-all check-kernel clean

all: check

check:
	@printf '#include "arena.h"\n#include "benchmark.h"\n#include "policy.h"\nint main(void) { return 0; }\n' | $(CC) $(CPPFLAGS) $(CFLAGS) -x c -fsyntax-only -
	@echo "Shared headers compile sucessfully."


EST_SRCS = $(wildcard tests/test_*.c)
TEST_NAMES = $(patsubst tests/%.c,%,$(TEST_SRCS))
TEST_BINS = $(addprefix bin/,$(TEST_NAMES))
ASAN_BINS = $(addprefix bin/asan/,$(TEST_NAMES))
TSAN_BINS = $(addprefix bin/tsan/,$(TEST_NAMES))

COMMON_SRCS = $(wildcard src/common/*.c)
POLICY_SRCS = $(wildcard src/policy/*.c)
LIB_SRCS = $(COMMON_SRCS) $(POLICY_SRCS)
HEADERS = $(wildcard include/*.h)

bin/%: tests/%.c $(LIB_SRCS) $(HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_SRCS) -o $@
 
bin/asan/%: tests/%.c $(LIB_SRCS) $(HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ASAN_FLAGS) $< $(LIB_SRCS) -o $@
 
bin/tsan/%: tests/%.c $(LIB_SRCS) $(HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TSAN_FLAGS) $< $(LIB_SRCS) -o $@

test: $(TEST_BINS)
	@set -e; for test in $(TEST_BINS); do \
		echo "Running $$test"; \
		./$$test; \
	done

test-asan: $(ASAN_BINS)
	@set -e; for test in $(ASAN_BINS); do \
		echo "Running $$test"; \
		./$$test; \
	done

test-tsan: $(TSAN_BINS)
	@set -e; for test in $(TSAN_BINS); do \
		echo "Running $$test"; \
		TSAN_OPTIONS=halt_on_error=1 ./$$test; \
	done

test-all: test test-asan test-tsan

check-kernel:
	@sh scripts/check_kernel.sh '$(LINUX_SRC)'

clean:
	rm -rf build bin
