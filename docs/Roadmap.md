# Support status and roadmap

dynarmic.js provides a source-built WebAssembly JIT backend for ARM32, Thumb and
AArch64, with Node.js hosts and a browser Worker example. It is suitable for
integration and experimentation when the limitations below fit the guest.
The project does not claim complete ARM architectural conformance or a general
performance lead over other emulators.

## Implemented and tested

- Shared A32/A64 emitter, bounded region selection, structured loops and direct
  block chaining.
- Callback, page-table and flat-window memory paths, explicit cache invalidation,
  stepping and halt handling.
- Native emitted-module semantic tests, deterministic guest/reference programs,
  Unicorn differential fuzzing and a replayable regression corpus.
- Native sanitizer checks, Node.js unit checks, and Chromium/Firefox Worker
  workflows. The tested Emscripten toolchain is 6.0.13.
- Optional per-instance profiling, fixed-kernel and cache benchmarks, browser
  reporting, and an external CoreMark workload protocol.
- A native static CMake package and source integration for Emscripten hosts.

A green check is evidence for the configuration and tests that ran. Release
validation must retain the exact revisions, build options and test results;
benchmark artifacts are only comparable under the [performance
protocol](Performance.md).

## Known limitations

- **Floating-point fidelity:** general inline arithmetic has not completed a
  strict audit of FPCR rounding, flush-to-zero/default-NaN behavior and FPSR
  exceptions. Known gaps include fixed-to-FP rounding, FMULX input-subnormal
  handling and the double-to-single ties-away approximation. Numeric min/max
  tests do not certify these other operations. Workloads requiring exact FP
  modes or exception behavior need additional validation.
- **Host model:** execution and module instantiation are synchronous and
  thread-confined. Async compilation, shared-memory/pthread execution and
  multi-worker atomic semantics are not supported.
- **Native execution:** the native package can emit Wasm and link consumers,
  but its runtime is a stub. Run generated code in an Emscripten host.
- **Guest environment:** this is a CPU backend. Applications supply memory,
  devices, operating system services and interpreter fallback. The browser
  example's semihosting ABI is a demonstration, not an operating system.
- **Memory callbacks:** cached CPU registers are not coherent inside memory
  callbacks. Observe or change them after returning from `Run()`, or through
  supported state-observing callbacks. See the [embedding contract](Embedding.md).
- **Cache capacity:** region sizes are bounded, but there is no configurable
  total code-cache ceiling or automatic eviction policy. Hosts can clear or
  invalidate code explicitly. Reported emitted bytes are not engine memory use.
- **Platform coverage:** browser CI covers Chromium and Firefox. Safari and
  other engine/version combinations need independent validation. Toolchain
  upgrades must revalidate the runtime's Emscripten table integration.

## Planned work

1. Extend strict floating-point tests and implement consistent slow/fast-path
   behavior for the remaining modes and exceptions.
2. Broaden application-level, full-duration and cross-browser measurements,
   including retained engine memory and populated-cache invalidation.
3. Improve distributable Emscripten consumer packaging and versioned host APIs.
4. Evaluate asynchronous compilation and hotness-based tiering with explicit
   cache ownership and cancellation contracts.

These are development directions, not promised release dates. Historical
milestones, rejected experiments and reproducibility records are retained under
[internals](../internals/README.md). Public usage contracts live in this
`docs/` directory and the source interfaces.
