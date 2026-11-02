# First guest in Node.js

Run a four-instruction ARM32 guest and an equivalent AArch64 guest. Each sets
register R0/X0 to `42`, prints it through the demo host's semihosting callback,
and exits. The example checks the halt reason, register value and exact output.
It is a short introduction to loading guest memory and calling the host API.

## Build and run

You need Node.js 22+, Emscripten, CMake, a C++20 compiler, GNU `patch` and Boost
headers. Install them using the [build guide](../../docs/GettingStarted.md).
The guest instructions are included in [hello.mjs](hello.mjs), so this example
does not need an ARM/AArch64 guest compiler or a native reference build.

From the repository root:

```sh
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-em -DCMAKE_BUILD_TYPE=Release \
  -DBoost_INCLUDE_DIR=/path/to/boost-headers \
  -DDYNARMIC_JS_TESTS=OFF
cmake --build build-em --target arm_jit -j2
node examples/node/hello.mjs
```

Missing guest-compiler warnings refer to the full browser demo; they do not
prevent this target from building. If reusing a build configured with
`DYNARMIC_JS_EXAMPLES_REQUIRE_GUESTS=ON`, set it to `OFF` for this minimal build.
Keep `arm_jit.js` and `arm_jit.wasm` in the same directory. For another build
directory, pass its `dist` directory as the only argument.

Expected output:

```text
ARM32: 0000002a (42) — PASS
AArch64: 0000002a (42) — PASS
```

The script exits with status `1` if loading, initialization, execution or any
result check fails. The first run includes JIT translation and Wasm compilation;
this example does not measure performance.

## What to change next

The [browser example](../browser/README.md) runs larger C guests in a Worker
and demonstrates memory modes and correctness checks against a native program.
Its [C API and semihosting contract](../browser/README.md#demo-c-api-and-semihosting)
explain the functions used here. The guest is a flat binary at `0x10000`, not an
ELF loader or an operating system. Execution is synchronous; run unbounded or
interactive workloads in a Worker with a termination policy.

To supply your own memory, devices or system calls, start from
[`arm_web.cpp`](../browser/arm_web.cpp) and the
[embedding guide](../../docs/Embedding.md). These example C/JavaScript APIs are
worked integrations, not a stable npm API.
