# Building and testing

dynarmic.js uses two build directories. The **Emscripten build** runs the JIT in
Node.js or a browser. The **native build** generates guest binaries, independent
reference programs and emitter tests; its runtime cannot execute guest code.
The [native performance comparison](Performance.md#native-dynarmic-comparison)
uses a third, independent build of upstream's native JIT.

## Dependencies

| Dependency | Purpose |
| --- | --- |
| Git, CMake 3.16+, C/C++20 compiler, GNU `patch` | All builds |
| Boost headers 1.57+ | Dynarmic headers; no compiled Boost library needed |
| Emscripten 6.0.13 | Tested toolchain for the WebAssembly host |
| Node.js 22+ | Execute generated Wasm and JavaScript tests |
| `arm-none-eabi-gcc`, `arm-none-eabi-objcopy` | ARM and Thumb guests, including the browser demo |
| Python 3.10+ with `ziglang` | AArch64 guest compiler, QuickJS build and source packaging |
| Python 3 with `unicorn` and `capstone` | Differential fuzzing and the strict regression gate |
| Chromium and Firefox | Browser Worker validation |

The complete `browser_page` target also builds QuickJS for ARM32 and Wasm. Its
first build downloads the checksum-pinned upstream source archive from
`bellard.org`; subsequent builds reuse the verified cache. The Node hello target
does not download or build QuickJS. See the [QuickJS example](../examples/quickjs/README.md)
for the standalone build and comparison protocol.
Debian/Ubuntu's ARM packages provide the newlib runtime notices automatically.
For another GNU ARM toolchain, set `QUICKJS_NEWLIB_LICENSE` to its complete
`COPYING.NEWLIB` or equivalent copyright inventory before building the page.

For example, on Debian or Ubuntu install the native tools with:

```sh
sudo apt-get install build-essential cmake ninja-build patch libboost-dev \
  gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi libnewlib-dev python3-venv
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install ziglang unicorn capstone
```

Install Emscripten and Node separately, then activate the Emscripten environment
in each shell used for a Wasm build. CMake checks Python modules at configure
time, so activate the virtual environment before configuring either build.

```sh
git clone --recurse-submodules https://github.com/xndcn/dynarmic.js.git
cd dynarmic.js
# For an existing checkout:
git submodule update --init --recursive
```

## Build the Emscripten host

Place Boost headers in a separate include directory. When using Debian/Ubuntu's
installed headers, one way to do that is:

```sh
mkdir -p .cache/boost-headers
cp -a /usr/include/boost .cache/boost-headers/
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-em -DCMAKE_BUILD_TYPE=Release \
  -DBoost_INCLUDE_DIR="$PWD/.cache/boost-headers" \
  -DDYNARMIC_JS_EXAMPLES_REQUIRE_GUESTS=ON
cmake --build build-em -j
```

Do not pass `/usr/include` as `Boost_INCLUDE_DIR` to Emscripten; that can mix
native headers with the Wasm toolchain's sysroot. A separately unpacked Boost
release is also suitable.

Run the example:

```sh
python3 -m http.server 8000 --bind 127.0.0.1 \
  --directory build-em/examples/browser/dist
```

Open <http://localhost:8000/> for the package quick start and browser benchmarks.
The standalone browser host tests run ARM32 and AArch64 versions of
the same program and compares their console output with a native reference.
See the [browser example guide](../examples/browser/README.md) for the host API,
memory modes and cancellation. The [Node.js example](../examples/node/README.md)
provides a smaller first run without guest cross compilers. To build just the
browser example and all its assets, use `cmake --build build-em --target browser_page -j2`.
Select a suite in **QuickJS benchmarks** and run a case, or choose the custom
LCG / sieve / π comparison. Both compare dynarmic.js, direct Wasm QuickJS and
the browser engine; expand **View benchmark code** to inspect their JavaScript.

## Build native tests and guests

```sh
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release
cmake --build build-native -j
ctest --test-dir build-native --output-on-failure
tests/regress.sh build-em build-native
```

The regression compares guest output across callback, page-table and
flat-window memory modes; it also runs runtime, emitted-IR and available
differential-fuzz tests. A successful run ends with `regression done fail=0`.
Read any `SKIP` messages: the default mode permits missing optional tools.

Use the fail-closed release gate when all dependencies are installed:

```sh
tests/regress.sh --strict build-em build-native
```

For a single guest:

```sh
node build-em/tests/jit_runbin.js build-native/tests/guest/neon.bin 1
```

For focused development commands, including corpus replay and sanitizers, see
[CONTRIBUTING.md](../CONTRIBUTING.md).

## Build options

| Option | Default | Effect |
| --- | --- | --- |
| `DYNARMIC_JS_TESTS` | `ON` | Build Emscripten hosts or native guests, reference programs and tests |
| `DYNARMIC_JS_EXAMPLES` | `ON` | Build the browser example in an Emscripten build |
| `DYNARMIC_JS_NPM_PACKAGE` | `OFF` | Build the standalone JavaScript package runtime with Emscripten; see the [npm release procedure](Releasing.md#npm-package) |
| `DYNARMIC_JS_EXAMPLES_REQUIRE_GUESTS` | `OFF` | Fail configure if any compiler needed for the complete browser demo is missing; enable for demo/release builds |
| `DYNARMIC_JS_PROFILE` | `OFF` | Publish per-instance `profile-v1` counters; adds generated-code instrumentation |
| `DYNARMIC_JS_REGION_DIAGNOSTICS` | `OFF` | Retain compile-boundary region histories; Emscripten only |
| `DYNARMIC_JS_BUILD_INTERNAL_TESTS` | `OFF` | Register historical experiment checks separately from the public tests |

For a library-only build, set both `DYNARMIC_JS_TESTS=OFF` and
`DYNARMIC_JS_EXAMPLES=OFF`. Keep production and profile builds in separate
directories. Region focus and other experimental settings are maintainer tools
documented in [internals](../internals/README.md), not stable tuning options.

## Troubleshooting

- **Missing submodule:** run `git submodule update --init --recursive`.
- **Missing guest or demo binary:** check the CMake error or warnings, install the ARM
  tools and Python `ziglang` in the active environment, then rerun configure.
- **WebAssembly validation error:** verify Node/browser support for tail calls
  and SIMD, and confirm that the host was linked with `dynarmic_js_host()`.
- **Browser fetch or Worker error:** serve the whole `dist` directory over HTTP;
  keep the JavaScript and Wasm files from the same build together. Review the
  browser console and your site's Content Security Policy.
- **Patch no longer applies:** confirm the pinned Dynarmic submodule revision.
  Portability patches apply to build-tree copies, never to the source submodule.
- **Unexpected compiler behavior after changing toolchains:** configure a new
  build directory. Do not reuse one CMake cache for native and Emscripten builds.
