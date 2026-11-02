# dynarmic.js

[![Unit checks](https://github.com/xndcn/dynarmic.js/actions/workflows/unit.yml/badge.svg)](https://github.com/xndcn/dynarmic.js/actions/workflows/unit.yml)
[![Browser Worker](https://github.com/xndcn/dynarmic.js/actions/workflows/browser.yml/badge.svg)](https://github.com/xndcn/dynarmic.js/actions/workflows/browser.yml)
[![GitHub Pages](https://github.com/xndcn/dynarmic.js/actions/workflows/pages.yml/badge.svg)](https://github.com/xndcn/dynarmic.js/actions/workflows/pages.yml)

Run ARM32 (ARM and Thumb) and AArch64 guest code in Node.js or a browser Web
Worker. dynarmic.js adds a WebAssembly JIT backend to
[Dynarmic](https://github.com/lioncash/dynarmic): it translates guest code into
small WebAssembly modules at runtime and links them through a shared function
table.

The project provides an [experimental JavaScript package](docs/JavaScript.md),
a C++ backend, an Emscripten host integration helper, and
[Node.js and browser examples](examples/README.md). C++ embedders supply guest
memory, devices and operating system services through Dynarmic's callbacks. See the
[support status and limitations](docs/Roadmap.md) before integrating it.
The project is currently experimental. The typed JavaScript API is versioned
`0.1.0-alpha.0`; pin the exact version while it evolves toward 1.0.

The [browser quick start](examples/browser/README.md) builds the same static
artifact used by Pages and serves it locally for inspection.

The private root npm workspace provides a repeatable artifact check and Vite
preview. CMake remains responsible for the engines and corresponding source.

## Quick start

Download a versioned npm tarball or the complete prebuilt browser site from
[GitHub Releases](https://github.com/xndcn/dynarmic.js/releases). The site archive
includes all demo engines: extract it and run `python3 -m http.server 8000` in
that directory, then open <http://localhost:8000/>. No compiler is needed.
If no release is available for your version, use the source build below.

The [package quick start](packages/dynarmic/README.md) provides
`import {createEmulator} from 'dynarmic.js'` for Node and browser Workers,
TypeScript declarations, and a separate Wasm asset for bundlers. The
[package release procedure](docs/Releasing.md#github-releases) explains the
versioned downloads, checksums and Pages deployment; npm registry availability
is not assumed.

To build the C++ backend and the complete browser demo from source:

You need CMake 3.16 or newer, a C++20 compiler, GNU `patch`, Boost headers 1.57
or newer, [Emscripten](https://emscripten.org/) (tested with 6.0.13), and Node.js
22.12 or newer for the Pages tooling (Node 24 LTS is used in CI).
The browser demo also needs `arm-none-eabi-gcc`,
`arm-none-eabi-objcopy`, a native C compiler, and Python with `ziglang` installed.

```sh
git clone --recurse-submodules https://github.com/xndcn/dynarmic.js.git
cd dynarmic.js
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-em -DCMAKE_BUILD_TYPE=Release \
  -DBoost_INCLUDE_DIR=/path/to/boost-headers \
  -DDYNARMIC_JS_EXAMPLES_REQUIRE_GUESTS=ON
npm ci
npm run build
npm run pages:preview -- --host 127.0.0.1
```

Open <http://127.0.0.1:4173/dynarmic.js/> for the package quick start, C benchmarks and
combined QuickJS benchmarks, with source code available inline. Vite serves the
generated directory without rewriting its engines or provenance files.
`python3 -m http.server` remains a dependency-free fallback. Use a browser
with WebAssembly tail calls and SIMD; current
Chrome and Firefox are exercised by CI. Boost headers for the Emscripten build
must live outside `/usr/include` so native system headers do not enter the
Wasm sysroot.

The [C benchmark lab](examples/c-bench/README.md) builds five scalar workloads
with one Clang version for ARM/dynarmic.js, x86/v86 and direct Wasm, without
libc. It separates loading and first-call costs from repeated execution,
verifies every checksum, and offers quick and extended runs with JSON export.

The page also includes a [QuickJS comparison](examples/quickjs/README.md): the
same JavaScript LCG, prime sieve and π workloads run inside ARM32 QuickJS through
dynarmic.js, QuickJS compiled directly to Wasm, and the browser's JavaScript
engine. It reports checked outputs, repeated samples and runtime statistics.
The first complete build downloads a checksum-pinned QuickJS source release;
later builds reuse the local cache. The source archive and MIT license ship with
the generated page.

The official JavaScript section also runs all eight V8 v7 suites (ten cases)
shipped in the pinned QuickJS extras archive. Choose one case or run every case
through all three engines. It retains the original one-second warmup and
measurement batches, at least 32 measured iterations, upstream checks, and
downloadable results including failures or cancellations.

For a smaller first run without guest cross compilers, start with the
[Node.js example](examples/node/README.md). The [build guide](docs/GettingStarted.md)
covers dependencies, the native test build, CMake options and troubleshooting. The
[browser example](examples/browser/README.md) documents the Worker protocol and
demo API.

## Features

- Shared ARM32, Thumb and AArch64 translation pipeline using upstream Dynarmic.
- Bounded multi-block regions, structured loops, direct block chaining, a return
  stack buffer and an indirect-branch cache.
- Callback memory, page-table fast paths and a contiguous flat-memory window.
- Explicit code-cache invalidation, single-step execution and guest halt reasons.
- Deterministic guest/reference tests, emitted-module tests and differential
  fuzzing against Unicorn.
- Optional per-instance profiling and reproducible benchmark protocols.

Execution is synchronous and confined to one thread. Browser hosts run in a
Worker so translation and guest execution do not block the page. Floating-point
mode and exception fidelity have [known gaps](docs/Roadmap.md#known-limitations).

## Build and test

The native build produces guest binaries, reference programs and emitter tests.
The Emscripten build executes the JIT:

```sh
cmake --build build-em -j2
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release
cmake --build build-native -j
ctest --test-dir build-native --output-on-failure
tests/regress.sh build-em build-native
```

The regular regression reports unavailable optional dependencies as skips. The
release gate requires the full guest, fuzzing and browser inputs:

```sh
tests/regress.sh --strict build-em build-native
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for focused checks, sanitizer builds and
the evidence required for compiler or performance changes. For measurements,
start with [the performance protocol](docs/Performance.md).

## Compare with native Dynarmic

On Linux x86-64 or AArch64, the native comparison builds the pinned upstream
Dynarmic CPU backend separately and runs the same 12 guest kernels on both
backends (ARM32, Thumb and AArch64; memory, call/return and scalar/NEON).
Both sides check the guest result on every run:

```sh
cmake -S tests/bench/native -B build-dynarmic-native -DCMAKE_BUILD_TYPE=Release
cmake --build build-dynarmic-native -j2
node tests/bench/native_run.mjs build-em/tests build-dynarmic-native \
  --runs 7 --output native-comparison.json
node tests/bench/native_validate.mjs native-comparison.json --require-comparison
```

Use a production Release Wasm build. The report retains cold and warm samples,
alternating process order, dispersion, executable hashes and build/CPU identity.
See [comparison methodology](docs/Performance.md#native-dynarmic-comparison) for
interpretation and limitations. `build-native` above tests the Wasm emitter;
`build-dynarmic-native` here executes upstream's native JIT.

## Integration

Use the [JavaScript API](docs/JavaScript.md) for a CPU with bounded guest RAM,
register access, stepping and host-handled supervisor calls. Its package README
includes runnable Node and Vite Worker examples.

Use Dynarmic's C++ `A32::Jit` or `A64::Jit` API and the
`dynarmic_js_host()` CMake helper to build an Emscripten host. The
[embedding guide](docs/Embedding.md) describes callback lifetime, memory access,
cache invalidation and deployment requirements. `examples/browser/arm_web.cpp`
is a complete host implementation.

The native build also installs a static CMake package:

```sh
cmake --install build-native --prefix /tmp/dynarmic-js-install
tests/install/smoke.sh build-native /tmp/dynarmic-js-install
```

This package supports native emission and linking checks. Its native runtime
is a stub: executing generated guest code requires an Emscripten host.

## Repository guide

| Path | Contents |
| --- | --- |
| `src/dynarmic/backend/wasm/` | WebAssembly emitter, runtime, dispatcher and code cache |
| `src/dynarmic/frontend/wasm/` | Frontend adapters using upstream instruction definitions |
| `externals/dynarmic/` | Pinned, unmodified upstream submodule |
| `patches/`, `cmake/` | Build-tree portability patches and host integration |
| `examples/` | Node.js integration, ARM32/AArch64 browser guests and a three-runtime QuickJS comparison |
| `packages/dynarmic/` | Typed JavaScript API, npm runtime host and verified source packaging |
| `tests/` | Public regression suite, fuzzers, benchmarks and debugging tools |
| `docs/` | Build, embedding, architecture and performance documentation |
| `internals/` | Maintainer handoffs, experiment-specific tests and measurement archives |

Start with the [architecture](docs/Architecture.md) for runtime ownership and
invariants, or [backend design notes](docs/WasmBackend.md) for emitter details.
Internal experiment archives preserve rejected candidates and historical data;
they are not part of the supported API.

## License and contributing

Project-owned integration and backend code is MIT. Upstream and
bundled dependencies retain their own licenses; see [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Bug reports and contributions are welcome through
[GitHub issues](https://github.com/xndcn/dynarmic.js/issues) and pull requests.
Please read [the contribution guide](CONTRIBUTING.md) before submitting changes.
See [SUPPORT.md](SUPPORT.md) for help, [SECURITY.md](SECURITY.md) for private
vulnerability reporting, [CHANGELOG.md](CHANGELOG.md) for changes, and the
[release guide](docs/Releasing.md) for distribution and validation.
