
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
- Linux 6.1 or newer at runtime (`MADV_COLLAPE`)
- GCC with C11 support (and the ASan/UBSan/TSan runtimes for `make test-asan` / `make test-tsan`)
- GNU Make
- Python 3 (for evaluation scripts)
- Linux perf (for hardware performance measurements)
- Google TCMalloc (for allocator comparison)

## Building

The project uses a Makefile.

```bash
make               # Build or validate the current project
make check         # Verify shared headers (including policy.h)
make test          # Build and run every tests/test_*.c
make test-asan     # Same tests under AddressSanitizer + UBSan
make test-tsan     # Same tests under ThreadSanitizer
make test-all      # test + test-asan + test-tsan
make check-kernel LINUX_SRC=path/to/linux  # Verify kernel assumptions
make clean         # Remove generated build files
```


Tests link all of `src/common/*.c` and `src/policy/*.c`, and are built
with `-pthread`. Binaries go to `bin/`, `bin/asan/` and `bin/tsan/`.
 
`make` itself only verifies that the shared headers compile. Benchmark
build targets will be added during implementation.


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

## Promotion Policy
 
`include/policy.h` and `src/policy/` decide which 2 MiB regions to
promote. The policy is pure logic: it receives first-touch events and
answers "keep tracking", "promote now" or "stop tracking". Only
`collapse.c` makes a syscall (`madvise(MADV_COLLAPSE)`); all
`userfaultfd` handling stays in the observer.
 
| File | Role |
|---|---|
| `include/policy.h` | Config, per-region state, API, statistics |
| `src/policy/policy.c` | Density/run thresholds, promotion budget, retry/backoff state machine |
| `src/policy/policy_config.c` | Parser for config spec strings |
| `src/policy/collapse.c` | One `MADV_COLLAPSE` per 2 MiB region, reports result to the policy |
| `tests/test_policy.c` | Unit, property and thread-race tests |
 
All API calls take one internal lock, so the fault handler and the
collapse workers may call the policy concurrently.
 
### Configuration
 
A policy is configured with a spec string of `key=value` pairs, parsed
by `promo_config_parse()`. Unspecified keys keep their defaults.
 
| Key | Values | Default | Meaning |
|---|---|---|---|
| `kind` | `never`, `always`, `threshold` | `threshold` | Policy family |
| `density` | pages `1..512`, or percent such as `25%` | `256` (50%) | Touched pages needed |
| `run` | pages `1..512` | `64` | Contiguous touched pages needed |
| `combine` | `density`, `run`, `and`, `or` | `density` | How the two tests combine |
| `budget` | integer, `0` = unlimited | `0` | Maximum regions held for promotion |
| `attempts` | `1..1000` | `3` | Collapse attempts per region |
| `backoff_ms` | `0..3600000` | `5` | Delay after first transient failure (doubles) |
| `backoff_max_ms` | `0..3600000` | `200` | Cap on the delay |
| `workers` | `1..1024` | `1` | Advisory: collapse worker threads |
 
```
kind=threshold,density=25%                       # promote at 128 touched pages
kind=threshold,density=12.5%,run=64,combine=or   # sparse but locally dense
kind=always                                      # promote every region on first touch
kind=never                                       # stop tracking at first touch, never promote
```
 
`kind=always` and `kind=never` run the same tracking and collapse path
as `threshold`, so they isolate the policy decision. They are not the
system baselines: "THP disabled", "always-THP" and khugepaged are
kernel settings, and `kind=always` still promotes through `MADV_COLLAPSE`.
 
### Observer contract
 
```c
/* fault handler */
promo_decision d = promo_policy_touch(&pol, region, page_idx, now_ns);
if (d == PROMO_NOW || d == PROMO_NEVER)
    /* UFFDIO_UNREGISTER exactly this 2 MiB range */;
if (d == PROMO_NOW)
    /* enqueue region for a collapse worker */;
 
/* collapse worker, when the region is due */
promo_next n = promo_run_collapse(&pol, region);
if (n == PROMO_NEXT_RETRY)
    /* requeue at region->retry_at_ns */;
```
 
- Region bases must be 2 MiB aligned; `promo_region_init()` rejects others.
- Register with `UFFDIO_REGISTER_MODE_MISSING` only, and resolve faults
  with `UFFDIO_COPY` from a zero-filled buffer. Do not use
  `UFFDIO_ZEROPAGE`: a region populated only by zero-page mappings
  cannot collapse (see below).
- Unregister the range before calling `promo_run_collapse()`.
- Faults already read from the userfaultfd when the range is
  unregistered still arrive; the policy counts them as `late_touches`
  and ignores them. Treat `EEXIST`, `ENOENT` and `EINVAL` from resolving
  them as harmless.
- Create the userfaultfd with `UFFD_USER_MODE_ONLY` (or use
  `/dev/userfaultfd`) unless the process has the privileges to trap
  kernel faults; `vm.unprivileged_userfaultfd` defaults to 0.
### Kernel assumptions
 
The policy's error handling depends on these properties of
`mm/khugepaged.c`:
 
| Assumption | Consequence in the code |
|---|---|
| `collapse_max_ptes_none()` returns 0 for a userfaultfd-armed VMA | A registered region with holes fails (`EBUSY`), so unregister first |
| `MADV_COLLAPSE` ignores khugepaged's `max_ptes_none` (`collapse_policy_madvise()` allows `HPAGE_PMD_NR`) | The density threshold is the only gate; a promoted region adds `(512 - touched) * 4 KiB` of zero-filled memory |
| `madvise_collapse()` returns only the errno of the last failed region | One region per call |
| No whole aligned 2 MiB region in the range: returns 0, does nothing | Bases are validated up front |
| `hugepage_vma_revalidate()` requires `vma_has_anon_rmap()` | Zero-page-only regions fail with `EINVAL`; resolve with `UFFDIO_COPY` |
| `madvise_collapse_errno()`: `ENOMEM` (no hugepage), `EBUSY` (cgroup charge or none-PTE limit), `EAGAIN` (page locked/pinned/off-LRU), anything else `EINVAL` | `ENOMEM`/`EBUSY`/`EAGAIN` retry with backoff; the rest give up |
| `MADV_COLLAPSE` is independent of the THP sysfs settings (kernel THP docs) | Works with `enabled=never`; not covered by `check-kernel` |
 
`make check-kernel` greps `../linux/mm/khugepaged.c` for all of these
except the last, and checks `MADV_COLLAPSE == 25` (the fallback value in
`collapse.c`). It exits non-zero if the kernel source no longer matches,
in which case the comments and retry classification in `src/policy/`
need a second look. A version other than 7.1 only produces a warning.
 
### Tests
 
`make test`, `make test-asan` and `make test-tsan` run
`tests/test_policy.c`, which covers:
 
- Run-length tracking across bitmap word boundaries, density and run
  thresholds, and the `and`/`or` combinations.
- `always`/`never`, the promotion budget (and its refund on give-up).
- The collapse state machine: retries, exponential backoff and cap,
  attempt exhaustion, permanent errors, invalid transitions.
- Late and duplicate touches, invalid indices, misaligned bases.
- A property test: the decision depends only on the set of touched
  pages, not the order.
- A thread race: eight threads cross the threshold together and exactly
  one receives `PROMO_NOW`.
- The config parser, including malformed specs.
These test the policy logic only. Tests against a real `userfaultfd`
and `MADV_COLLAPSE` need the observer and are not written yet.
 
### Limitations
 
- The policy only sees first touches. Density means "populated", not
  "reused", and a region that never reaches its threshold stays
  registered and keeps paying a fault per page (there is no timeout).
- There is no demotion path for a region promoted in an earlier phase.
- `budget` counts regions, not bytes of RSS.


## References

- Linux kernel documentation: Transparent Hugepage Support
- Linux manual: userfaultfd(2)
- Linux manual: madvise(2)
- Temeraire: Hugepage-Aware Allocator (OSDI 2021)

