
# CS-477 Project 3: Fault-Guided Selective Huge Pages

Advanced Operating Systems — EPFL, Fall 2026

## Overview

This project investigates whether first-touch memory access patterns
can guide the selective promotion of regular 4 KiB pages into 2 MiB
Transparent Huge Pages (THPs).

The goal is to achieve performance comparable to Linux's automatic
THP management while reducing unnecessary huge-page usage and
memory overhead.

The project uses Linux's `userfaultfd` interface to track first
accesses and `MADV_COLLAPSE` to request huge-page promotion.

## Project Components

### 1. Benchmarks and Application Analysis

Implement three benchmarks with different memory access patterns:
- Sequential memory access
- Random memory access
- Phase-changing memory access

Analyze TLB behavior and sensitivity to huge pages.

### 2. Fault Handler and Promotion Policy

- Track first page accesses using `userfaultfd`.
- Record page density and contiguous access patterns.
- Implement basic and configurable promotion policies.
- Promote eligible regions using `MADV_COLLAPSE`.
- Test correctness and concurrent access handling.

### 3. Performance Evaluation

Compare four memory management approaches:
- THP disabled
- Automatic THP allocation
- Background promotion using khugepaged
- Custom fault-guided promotion policy

Measure runtime, memory usage, THP coverage, TLB misses,
and promotion overhead.

### 4. Memory Allocator Comparison

Compare glibc malloc and Google TCMalloc using an
allocation-heavy workload under identical THP settings.

## Repository Structure

- `include/` — Shared interfaces and data structures
- `src/common/` — Shared memory allocation utilities
- `src/benchmarks/` — Benchmark implementations
- `src/observer/` — Fault handler and promotion worker (planned)
- `src/policy/` — Promotion policies (planned)
- `scripts/` — Experiment automation
- `configs/` — Experiment configurations
- `tests/` — Correctness and integration tests
- `results/` — Experimental results
- `docs/` — Project documentation

## Requirements

- Linux with Transparent Huge Pages and userfaultfd support
- GCC with C11 support
- GNU Make
- Python 3 (for evaluation scripts)
- Linux perf (for hardware performance measurements)
- Google TCMalloc (for allocator comparison)

## Building

The project uses a Makefile.

```bash
make          # Build or validate the current project
make check    # Verify shared headers
make clean    # Remove generated build files
```

Currently, `make` only verifies that the shared headers compile.
Benchmark build targets will be added during implementation.

## Development

All benchmarks will share a common memory arena interface.

Memory arenas will use private anonymous mappings aligned to
2 MiB boundaries. Their initial population will use 4 KiB pages
for experiments involving subsequent promotion.

The benchmarks will be designed to operate independently of
the fault handler, allowing the same workloads to be reused
across different memory management configurations.

## References

- Linux kernel documentation: Transparent Hugepage Support
- Linux manual: userfaultfd(2)
- Linux manual: madvise(2)
- Temeraire: Hugepage-Aware Allocator (OSDI 2021)

