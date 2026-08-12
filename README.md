# monero-stratum-pow-verifier

A small native C API around RandomX for bounded, asynchronous proof-of-work
verification. The public boundary is one opaque header; RandomX types and the
verifier's C++ internals are not exposed to callers.

This tree is an early `0.1.x` implementation intended for local review and
integration testing. It does not contain or copy code from XMRig or XMRig
Proxy.

## What is included

- `include/monero_stratum_pow_verifier.h`: the only public header
- `src/monero_stratum_pow_verifier.cpp`: the verifier implementation
- `examples/verify.c`: a complete C example
- `tools/benchmark.cpp`: a public-API light/fast benchmark
- `tests/`: C ABI, lifecycle, concurrency, bounds, cancellation, and known-answer tests
- CMake static-library, install, and package-target support

The library computes the authoritative raw 32-byte RandomX hash and can compare
it with a claimed hash. Stratum job correlation, nonce placement, block target,
difficulty, and share accounting intentionally remain in the caller.

## Pinned RandomX version

The submodule is the official
[`tevador/RandomX`](https://github.com/tevador/RandomX) repository at:

```text
tag:    v1.2.2
commit: 6c4340ba4561aec9a3611c1aedf9931239777fb3
```

This is also the exact RandomX gitlink used by Monero `v0.18.5.1`. CMake checks
the revision when Git metadata is available; source archives without Git
metadata build from the vendored files as supplied.

After cloning:

```sh
git submodule update --init --recursive
```

## Resource model

The caller chooses the memory mode once per verifier context. Every resident
seed has one shared cache or dataset and one private RandomX VM per verification
worker. A VM is used only by its owning worker.

| Mode | Approximate resident memory per seed | Behavior |
|---|---:|---|
| `MSPV_MEMORY_LIGHT` | `256 + 2 × workers` MiB | Keeps the RandomX cache; lower memory and slower hashing |
| `MSPV_MEMORY_FAST` | `2080 + 2 × workers` MiB | Keeps the full dataset; higher memory and faster hashing |

Preparing a fast seed temporarily holds the additional 256 MiB cache. Its peak
is therefore at least about 2336 MiB before VM scratchpads. Multiple resident
seeds multiply these figures.

`max_seeds` defaults to **2** and is configurable. There are no hard-coded
“previous/current/next” slots. Activation is only a caller-visible designation;
every submission explicitly names the exact seed it needs. This permits late
shares without coupling the library to one stratum rotation policy.

Seed preparation is serialized to make peak memory predictable. Fast dataset
construction uses `seed_init_threads` internally. Release rejects new work for
that seed, lets already-admitted work finish, destroys its memory, and only then
allows `mspv_seed_wait_released()` to return.

## Build

Requirements are CMake 3.16 or newer, a C++17 compiler, a C compiler for the
example, and platform threads.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The normal test suite uses light mode. The optional full-memory test needs more
than 2.3 GiB available during seed preparation:

```sh
cmake -S . -B build-fast \
  -DCMAKE_BUILD_TYPE=Release \
  -DMSPV_BUILD_FAST_TESTS=ON
cmake --build build-fast --parallel
ctest --test-dir build-fast --output-on-failure -L fast
```

Useful CMake options:

| Option | Standalone default | Purpose |
|---|---:|---|
| `MSPV_BUILD_TESTS` | `ON` | Build C/C++ tests |
| `MSPV_BUILD_EXAMPLES` | `ON` | Build `mspv_verify` |
| `MSPV_BUILD_BENCHMARK` | `ON` | Build `mspv_benchmark` |
| `MSPV_BUILD_FAST_TESTS` | `OFF` | Enable the high-memory integration test |
| `MSPV_ENABLE_TRACE_LOGGING` | `OFF` | Compile per-job TRACE diagnostics |
| `MSPV_ENABLE_SANITIZERS` | `OFF` | Add AddressSanitizer and UndefinedBehaviorSanitizer |

Tests, examples, and the benchmark default to `OFF` when this project is added
as a subdirectory of another CMake project.

## Integrate it into another project

The provided target is an explicit **static library**:

```cmake
add_subdirectory(path/to/monero-stratum-pow-verifier)
target_link_libraries(your_target PRIVATE mspv::verifier)
```

Application code includes only:

```c
#include <monero_stratum_pow_verifier.h>
```

The installed CMake target carries the required RandomX archive and thread
dependency. It is two static archives rather than one physically merged
archive, but consumers link only `mspv::verifier` and do not need a RandomX
header. The ABI is C, but the static implementation is C++; a CMake consumer
must enable the `CXX` language (as well as `C` for a C application), or use a
C++ linker driver so the C++ runtime is linked.

You can also copy `include/`, `src/`, and the pinned RandomX submodule into a
project and compile them there. A literal single-header implementation would
still have to compile all of RandomX and would not make hashing faster or safer.
Static-library calls are negligible beside a RandomX hash, and link-time
optimization can still be enabled by the parent build. The opaque library/API
boundary is useful because stratum code cannot accidentally depend on verifier
or RandomX internals; static versus shared linkage is not itself a security
boundary.

For a system installation:

```sh
cmake --install build --prefix /your/prefix
```

Then consume it with:

```cmake
find_package(monero-stratum-pow-verifier 0.1.0 EXACT CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE mspv::verifier)
```

## Minimal API flow

1. Initialize and customize `mspv_config`.
2. Create and start an `mspv_context`.
3. Prepare a RandomX seed key and wait for it to become ready.
4. Submit copied hashing blobs with `mspv_verify_submit()` or
   `mspv_hash_submit()`.
5. Drain `mspv_completion` records; completion order is not guaranteed.
6. Release obsolete seeds and shut down the context.

`examples/verify.c` implements the entire flow. For the upstream simple test
vector:

```sh
./build/mspv_verify light \
  74657374206b657920303030 \
  5468697320697320612074657374 \
  639183aae1bf4c9a35884cb46b09cad9175f04efd7684e7262a0ac1c2f0b4e3f
```

Change `light` to `fast` to use a full dataset.

## Configuration defaults and bounds

| Setting | Default | Allowed | Meaning |
|---|---:|---:|---|
| `worker_count` | `min(4, hardware threads)` | 1..256 | Verification workers and VMs per seed |
| `seed_init_threads` | same as workers | 1..256 | Fast dataset construction threads |
| `pending_capacity` | 256 | 1..1,000,000 | Copied jobs waiting for a worker |
| `max_outstanding` | 512 | `pending_capacity`..1,000,000 | Accepted jobs not yet polled |
| `max_input_size` | 4096 bytes | 1 byte..64 MiB | Largest accepted hashing blob |
| `max_buffered_input_bytes` | 16 MiB | `max_input_size`..16 GiB | Aggregate copied payload bytes, including running jobs |
| `max_seed_key_size` | 60 bytes | 1..60 bytes | Portable RandomX key maximum; Monero seeds are 32 bytes |
| `max_seeds` | 2 | 1..64 | Resident/preparing/releasing seeds |
| `memory_mode` | light | light or fast | Cache or full dataset |
| `large_pages` | try | off, try, or require | Fall back with a warning unless pages are required |
| `options` | secure JIT | documented flag mask | JIT/AES implementation choices; disable and secure JIT conflict |
| `log_level` | info | error..trace | Highest diagnostic level delivered to the sink |

All values are validated against conservative implementation ceilings before
threads or RandomX resources are created. A logical pending-admission slot, an
outstanding slot, and the copied-payload byte budget are reserved before caller
data is copied, so concurrent submissions cannot race past those limits. The
completion ring is allocated to `max_outstanding` at context creation.
`max_buffered_input_bytes` bounds copied payload bytes; normal container,
allocator, thread-stack, VM, and queue-object overhead is additional.

## Diagnostics

Set `config.log` to receive ERROR, WARNING, INFO, and DEBUG lifecycle/resource
messages. TRACE adds one record per accepted/completed job and is compiled out
unless `MSPV_ENABLE_TRACE_LOGGING=ON`. Diagnostics include mode, worker counts,
seed IDs, allocation fallbacks, timings, queue limits, shutdown disposition,
and invariant failures. Seed-key bytes, hashing blobs, and hashes are never
logged.

The log sink and completion notifier may be called concurrently from verifier
threads. They must be fast, must not throw, and must not call another MSPV
function on the same context. User callbacks are never invoked while a verifier
mutex is held. Notifier hints cover completion-queue empty-to-nonempty edges,
seed preparation resolving, and owned-thread failure; drain completions and
query tracked seeds after a hint. Callback storage must remain valid until
`mspv_shutdown()` or `mspv_destroy()` returns. Shutdown also waits for other
concurrently executing context calls before returning.

## Benchmark

The benchmark uses only the installed public API and first checks an official
known answer. Timed jobs call `mspv_verify_submit()`, vary a nonce on every
input, and use an all-zero claimed hash. Each timed completion must report a
successful calculation and the expected mismatch; timed hashes are not
individual known-answer vectors. Queue priming is included in the timed
interval. The in-flight limit and retained latency samples are bounded, and a
finite no-progress watchdog prevents silent hangs.

```sh
./build/mspv_benchmark --mode light --seconds 15

./build/mspv_benchmark \
  --mode fast \
  --workers 4 \
  --init-threads 8 \
  --inflight 16 \
  --seconds 30 \
  --large-pages try \
  --verbose
```

It reports seed preparation time, completed verifications per second, and
queue/hash/total latency averages plus p50, p95, and p99. Compare results only
when mode, worker count, JIT/AES policy, large-page outcome, in-flight window,
CPU, and memory configuration match.

## Security and lifecycle notes

- Input is rejected when null, empty, oversized, over queue capacity, or over
  the aggregate byte budget. Accepted input and claimed hashes are copied.
- Claimed hashes are compared in constant time. A mismatch is a successful hash
  calculation and the computed raw hash is still returned.
- Each `(seed, worker)` pair owns a separate VM. Immutable cache/dataset memory
  is shared only after complete initialization.
- Light and fast modes produce identical consensus hashes; the tests exercise
  the same known answer in both modes.
- `MSPV_SHUTDOWN_CANCEL_PENDING` cancels queued work; an already-running
  RandomX hash cannot be interrupted. Fast cache initialization is also
  non-interruptible, while dataset cancellation is cooperative between chunks.
- All context API calls are synchronized except `mspv_destroy()`, which requires
  exclusive ownership after other calls return.
- Large pages and JIT are performance/hardening choices, not consensus inputs.

These controls reduce common integration hazards but do not replace application
validation, target checking, fuzzing, or an independent security review.

## License

The wrapper is MIT licensed. RandomX is BSD 3-Clause licensed; see
`THIRD_PARTY_NOTICES.md` and `third_party/RandomX/LICENSE`.
