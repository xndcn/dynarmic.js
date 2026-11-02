# Dynarmic native comparison

This standalone CMake project builds the repository's five fixed benchmark
hosts against the **unmodified pinned upstream Dynarmic native backend**.
It supports Linux x86_64 and arm64 hosts. A native build of the repository root
(`build-native`) builds the Wasm emitter for tests; it is not this comparator.

Dependencies are CMake, a C++20 compiler, Boost headers, the initialized
`externals/dynarmic` submodule and a production Emscripten build. No additional
benchmark dependency or downloaded guest is needed.

For an independent audit of the throughput denominators, install Unicorn and
run `python3 tests/bench/check_instruction_counts.py`. It counts the actual
embedded guest streams through SVC; the CI comparison job requires this check.

```sh
cmake -S tests/bench/native -B build-dynarmic-native -DCMAKE_BUILD_TYPE=Release
cmake --build build-dynarmic-native -j2
ctest --test-dir build-dynarmic-native --output-on-failure
cmake --build build-em --target jit_bench jit_thumb_bench jit_a64_bench jit_a64_simd_bench jit_callbench -j2
node tests/bench/native_run.mjs build-em/tests build-dynarmic-native \
  --runs 5 --output native-comparison.json
node tests/bench/native_validate.mjs native-comparison.json --require-comparison
```

The 12 cases cover A32, Thumb and A64 memory through callbacks, page tables and
flat memory; A32 call/return; and A64 scalar/NEON arithmetic. Both implementations
compile the same guest code and correctness checks from the same host files.
All accessed addresses are within the allocated RAM. Native fastmem and the
Wasm checked flat window implement that common in-range workload; their fault
handling and access checking differ and are not compared by these kernels.

Each pair starts one fresh process per implementation, alternates their order,
and retains one cold and eight warm Runs. Case order rotates each round. The
Run timer excludes construction, reset and result checking. Cold Run time
includes guest execution and translation. Warm Wasm observations include engine
tiering; no observation is discarded. Native compilation phase counters are
`null`, because upstream has no equivalent instrumented interface.

The artifact retains raw stdout/stderr, exit status, individual samples,
per-process warm medians, pair ratios and median absolute deviations, binary
and workload hashes, compiler flags, upstream revision, CPU and runtime data.
`nativeOverWasmThroughput = wasm milliseconds / native milliseconds`: values
above one favor native. Five or more fresh process pairs permit the
`comparison` qualification; fewer pairs are explicitly `smoke-only`. This is
an observation-count qualification, not a guarantee of low noise or steady
state. There is no native speed threshold in CI.

Use a quiet host, a fixed CPU affinity and the same runtime options for all
observations. Inspect dispersion and all eight warm indices. Very short Thumb
kernels are especially sensitive to noise. These are microkernels, not an
application, browser, CoreMark or overall emulator performance score. They do
not measure peak memory, native emitted-code size or native compile phases.

The collector rejects modified upstream sources, changed binaries or workload
sources during collection, failed correctness checks, and profile, region
diagnostic or test-exception builds. Output files are never overwritten.
Collection failures retain raw diagnostics in `<output>.failure.json`; that
partial artifact is not a benchmark report. Build metadata describes the
configured build and the checkout observed during collection; it is not a
build-time source attestation. Rebuild both implementations before collection.
