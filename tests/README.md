# Testing dynarmic.js

Run commands from the repository root. See [Getting started](../docs/GettingStarted.md)
for toolchain installation and native/Emscripten build commands.

## Fast checks

The JavaScript benchmark protocol tests use Node.js 22 or newer and need no
compiled artifacts or npm packages:

```sh
node --test tests/bench/*.test.mjs
```

After configuring and building the native tree, run its registered tests:

```sh
cmake --build build-native --parallel 2
ctest --test-dir build-native --output-on-failure
```

Native C++ tests check decoder, cache, region planner, and selector invariants.
With Node available at configure time, CTest also runs CMake validation,
benchmark protocols, emitted-Wasm IR edge cases, and the region corpus. The
native library can emit Wasm for these tests; guest execution uses the
Emscripten build.

CTest also runs the installed-package consumer and all native-emitted smoke,
saturating arithmetic, binary encoding and IR edge tests. The sanitizer build
runs the same suite, including its exported-library consumer.

`node build-em/tests/jit_callbench.js board` runs the same checked call/return
kernel with cycle-counted board regions and entry sharing. Omitting `board`
keeps the ordinary RSB workload used by the fixed benchmark suite.

## Full regression

Build both trees before running the end-to-end gate:

```sh
tests/regress.sh --strict build-em build-native
```

This checks A32, Thumb, and A64 guest execution, reference checksums and logs,
memory modes, self-modifying code, JIT lifecycle, emitted IR, benchmarks, browser
hosts, and differential fuzzing. Strict mode fails when a required tool or
artifact is absent. Without `--strict`, optional browser, guest, and fuzz checks
can be skipped; a successful non-strict run therefore has narrower coverage.
Failures retain their temporary artifacts and print the directory path. To
retain successful runs too, choose a new evidence directory (its parent must
exist):

```sh
DYNARMIC_JS_TEST_ARTIFACTS="$PWD/test-results/release-check" \
  tests/regress.sh --strict build-em build-native
```

Existing directories and symlinks are refused. Evidence includes command
arguments, working directories, stdout/stderr and exit statuses, guest outputs,
benchmark JSON, and a separate log and mismatch corpus for each seeded fuzz
campaign. CI uploads this evidence even on failure; one failed campaign cannot
have its reproducer overwritten by the next campaign.
Fuzz summaries distinguish compared cases from un-compared host statuses.
Empty campaigns, zero comparisons and excessive un-compared fractions fail;
the [campaign gates](fuzz/README.md#campaign-coverage-gates) document the limits.

The [fuzz corpus guide](fuzz/README.md) explains reduced-case replay and Unicorn
campaigns. [CoreMark](bench/coremark/README.md) has a separately built workload
and is included with `--coremark WORKLOAD_DIR`.

## Test layout

| Directory | Purpose |
| --- | --- |
| `unit/` | Native C++ invariants and algorithm properties |
| `cmake/` | Build-option validation |
| `ir/` | Native emission and Node execution against independent references |
| `hosts/` | Emscripten JIT hosts, runtime checks, and benchmark workloads |
| `guest/`, `guest64/` | Bare-metal ARM/AArch64 guests and native oracles |
| `a32/`, `a64/`, `fuzz/` | Differential fuzzing and retained semantic cases |
| `bench/` | Reusable collectors, validators, comparisons, and their tests |
| `browser/` | Real Chromium interaction, cancellation, recovery and layout checks for the example |
| `tools/` | Decoder/IR diagnostics and generated-Wasm corpus checks |
| `install/` | Installed CMake package consumer smoke test |
| `npm/` | Installed npm tarball, Node/Worker/bundler/types, integrity and corresponding-source rebuild checks |

For install validation, run `tests/install/smoke.sh build-native`. Performance
reporting methodology is in [Performance](../docs/Performance.md); passing a
correctness benchmark does not establish a performance improvement.

The [native Dynarmic comparison](../docs/Performance.md#native-dynarmic-comparison)
has an independent CMake build at `bench/native/`, reuses all fixed guest
kernels, and records interleaved native/Wasm measurements and raw output.

Historical experiment-specific tools and evidence checks live in
[`internals/`](../internals/README.md) and are opt-in. Public tests must not depend
on those archived files.
