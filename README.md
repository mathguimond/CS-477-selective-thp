
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


## Benchmarks

The project includes three benchmarks:
- **Microbenchmark:** Configurable memory access patterns, density, and working-set size.
- **BFS:** Breadth-First Search from the GAP Benchmark Suite (GAPBS).
- **PageRank:** PageRank from GAPBS.

### Setup

Clone the repository with its submodules:

```bash
git clone --recurse-submodules https://github.com/mathguimond/CS-477-selective-thp.git project
cd project
```

If already cloned:

```bash
git submodule update --init --recursive
```

Apply our GAPBS modifications (2 MiB-aligned allocations):

```bash
cd third_party/gapbs
git apply ../../patches/gapbs-alignment.patch
cd ../..
```

Compile the benchmarks:

```bash
make -C third_party/gapbs -j2 bfs pr

gcc -O2 -pthread -Iinclude -D_GNU_SOURCE -std=c11 \
    -Wall -Wextra -Wpedantic \
    src/common/arena.c src/benchmarks/sequential.c \
    -o /tmp/sequential
```

### Running Benchmarks

All benchmarks can be executed from the project root using `scripts/run_benchmark.py`.

**Microbenchmark:**

```bash
python3 scripts/run_benchmark.py --benchmark micro \
    --memory-mib 128 --threads 2 --density 25 --pattern random
```

Additional options:
- `--hot-density`: Density during the hot phase (default: same as cold density; must not exceed it).
- `--reps`: Number of hot-phase repetitions (default: 100).
- `--pattern`: `sequential` or `random`.

**BFS:**

```bash
python3 scripts/run_benchmark.py --benchmark bfs \
    --graph-scale 18 --threads 2
```

**PageRank:**

```bash
python3 scripts/run_benchmark.py --benchmark pagerank \
    --graph-scale 18 --threads 2
```

For GAPBS, `--graph-scale N` generates a graph with approximately 2^N vertices. The default is 18.

### Allocator Selection

The runner supports two allocators:

```bash
--allocator glibc
--allocator tcmalloc
```

Example:

```bash
python3 scripts/run_benchmark.py --benchmark bfs \
    --graph-scale 18 --threads 2 --allocator tcmalloc
```

TCMalloc requires:

```bash
sudo apt install libtcmalloc-minimal4
```

The allocator comparison is primarily intended for BFS and PageRank, since the microbenchmark allocates its main arena directly using `mmap()`.

### Notes for Integration

- GAPBS graph adjacency arrays have been modified to use 2 MiB-aligned allocations.
- Initial 4 KiB page backing was verified for both BFS and PageRank.
- The benchmarks currently run independently of the `userfaultfd` handler.
- Future integration requires registering monitored memory regions before their first accesses and safely unregistering them before deallocation.
- Hardware TLB-miss counters are unavailable in our VirtualBox VM. Final performance measurements should use a machine supporting hardware performance counters.
- GAPBS modifications are stored in `patches/gapbs-alignment.patch`, rather than committed directly to the upstream submodule.


## References

- Linux kernel documentation: Transparent Hugepage Support
- Linux manual: userfaultfd(2)
- Linux manual: madvise(2)
- Temeraire: Hugepage-Aware Allocator (OSDI 2021)

