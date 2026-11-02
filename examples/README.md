# Examples

These examples run real ARM guest instructions through dynarmic.js and verify
the results. Start small, then explore the Worker host and its C++ callbacks.

| Example | Learn | Requirements beyond the core build |
| --- | --- | --- |
| [First guest in Node.js](node/README.md) | Load ARM32/AArch64 instructions, run the JIT, inspect registers and console output | Node.js 22+; no guest compiler |
| [Package quick start and browser benchmarks](browser/README.md) | Serve the Pages site: install the release package, compare C and QuickJS workloads across execution paths, and reuse the documented ARM32/AArch64 Worker host | ARM cross compiler, Python `ziglang`, native C compiler, current Chrome or Firefox |
| [Standalone C kernels for three targets](c-bench/README.md) | Build the same five freestanding kernels as ARM32, x86 and direct WebAssembly with one pinned compiler, then check every call's checksum against an independent Python reference | Python 3.10+ with the `ziglang` package; a browser only for measurements |
| [QuickJS across three runtimes](quickjs/README.md) | Run identical JavaScript LCG, sieve and π workloads in an ARM32 QuickJS guest, direct Wasm QuickJS and the host JavaScript engine | ARM cross compiler, Emscripten, Python 3; first build downloads checksum-pinned QuickJS sources |

The QuickJS example also includes the complete browser-compatible V8 v7 suite
from QuickJS extras: eight suites, ten selectable cases, original timing windows
and correctness checks across the same three execution paths.

The [build guide](../docs/GettingStarted.md) covers common dependencies. Commands
in the example guides run from the repository root. The ARM examples share
[`browser/arm_web.cpp`](browser/arm_web.cpp), a 16 MiB bare-metal host with
explicit memory callbacks and SVC-based semihosting; the C benchmark example
reuses that host for its ARM32 guest and drives the same kernels through x86
and direct WebAssembly references.

Examples demonstrate integration patterns and are covered by runtime and
browser checks. They do not provide an operating system, a general-purpose
binary loader or a stable JavaScript package. See the
[embedding contract](../docs/Embedding.md) before adapting the host, and the
[performance protocol](../docs/Performance.md) before making benchmark claims.

Contributors can use the [example maintenance guide](MAINTAINING.md) for
acceptance criteria and the upstream practices behind these examples.
