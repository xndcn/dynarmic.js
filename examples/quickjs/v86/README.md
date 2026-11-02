# Compare QuickJS through v86 and dynarmic.js

This is a local development measurement harness for the ten official V8 v7
JavaScript cases shipped with QuickJS extras. It compares bare-metal x86
QuickJS through v86 with bare-metal ARM32 QuickJS through dynarmic.js. It is
separate from the public Pages demo. The
[2026-10-31 measured comparison](../../../internals/measurements/2026-10-31-quickjs-v86/README.md)
found a median equal-suite ratio of **1.73×** in favor of dynarmic.js on the
tested machine, with case medians of **1.27×–1.92×**. That report includes the
browser interruptions, paired reruns and complete raw evidence.

Both guests use the pinned QuickJS **2025-09-13** source, **O2**, switch dispatch,
no LTO, `-fwrapv`, `-fno-strict-aliasing`, the same `EMSCRIPTEN` platform guards,
allocator and JS evaluation code. Both have **256 MiB guest RAM** and a
**192 MiB QuickJS allocation limit**. The x86 application permits compiler
builtins like ARM; it links only its own newlib and libgcc, with no Linux guest
or glibc. Its Pentium 4 build uses SSE2 C arithmetic (`-msse2 -mfpmath=sse`),
without fast-math. The standard i386 ABI still uses x87 for some returns, and
newlib contains x87 assembly; this is not an x87-free build.

The x86 port changes the shared runner's clock transport to an I/O read at
`0xE100`; output and exit use `0xE104` and `0xE108`. Both hosts supply monotonic
wall-clock microseconds from `performance.now()`, using the same wrapping
32-bit clock extension in the guest. Guest virtual CPU time is not the timer.
The official harness retains its one-second warmup and one-second measurement
batches until at least 32 calls complete. v86 uses its normal asynchronous
scheduling with its JIT enabled.

## Build and freeze inputs

First build the [browser example](../../browser/README.md) with the baseline
O2/ARM/switch settings and all ARM tuning switches off. The collector rejects
a tuned ARM guest. You also need Python 3.10+, Node.js 22+, Chromium, GNU make,
binutils and **GCC 11 with i386 multilib support** (`gcc-11-multilib` on
Debian/Ubuntu). A working `gcc-11 -m32` libgcc is required. The build downloads
and compiles newlib locally with at most four jobs; it does not install a
system toolchain.

Pinned dependencies:

- [newlib 3.3.0](https://sourceware.org/pub/newlib/newlib-3.3.0.tar.gz), SHA-256
  `58dd9e3eaedf519360d92d84205c3deef0b3fc286685d1c562e245914ef72c66`.
- Official npm [v86 0.5.473](https://registry.npmjs.org/v86/-/v86-0.5.473.tgz),
  package version `0.5.473+g315b2b0`, revision
  `315b2b0b758703ef0e6cc7b4088d6068f83f3c20`. `prepare.py` checks its pinned
  SHA-512 integrity and supplies the exact `v86.wasm`, without a fallback binary.

Run from the repository root. Use new preparation and result directories for
each experiment; the commands below assume those directories do not exist.
Complete all builds before measuring.

```sh
python3 examples/quickjs/v86/build.py \
  --output build-v86-quickjs --base-dist build-em/examples/browser/dist \
  --cc gcc-11 --jobs 2
python3 examples/quickjs/v86/prepare.py \
  --arm-dist build-em/examples/browser/dist \
  --guest build-v86-quickjs/guest --cache build-v86-quickjs/runtime \
  --out build-v86-quickjs/browser-dist-review
node examples/quickjs/v86/compare.mjs \
  --dist build-v86-quickjs/browser-dist-review \
  --browser /usr/bin/google-chrome --cases all --repeats 3 --timeout 600000 \
  --output build-v86-quickjs/comparison-review
node examples/quickjs/v86/analyze.mjs \
  build-v86-quickjs/comparison-review \
  --output-prefix build-v86-quickjs/comparison-review-summary
```

`quickjs-x86-manifest.json` records source and library hashes, compiler flags,
derived transport code and licenses. `comparison-inputs.json` freezes both
emulators and the shared benchmark sources. The collector verifies those hashes
and its own code throughout measurement. Keep them unchanged while a run is
active. Resume an interrupted run with the same comparison command plus
`--resume`; preserve its existing observations. Analysis writes JSON, Markdown
and CSV to a new prefix and reports missing or failed cases explicitly.

## Read the comparison

An uninterrupted three-round run uses **three fresh Chromium processes**, with
a fresh Worker and emulator for each case/runtime: 10 cases × 2 runtimes × 3 rounds. Case order
rotates and emulator order alternates. Upstream iterations within a Worker are
not additional independent rounds.

Resuming an interrupted round creates another browser process. If interruption
split a pair, retain the completed half under `superseded/` with its original
hash and a recovery record, then rerun both halves together. Do not mix the old
half with a new browser attempt or overwrite its original bytes. The measured
comparison needed two resumes and used five browser processes; all 30 final
pairs were completed within a single process each.

The ratio is **v86 milliseconds per iteration / dynarmic milliseconds per
iteration**; above 1 means dynarmic is faster in that paired observation.
Within each round, Crypto's two cases and EarleyBoyer's two cases are combined
geometrically, then all eight suites receive equal weight. Round ratios are
summarized after pairing. A final aggregate requires every requested case and
round to pass. `loadMs` and complete-call `runMs` remain separate diagnostics:
adaptive iteration counts make whole-call durations unsuitable for comparing
a fixed amount of work.

These are complete execution-path measurements. ISA, compiler versions, libc
builds, ABI, floating-point paths, JIT strategy and scheduling differ between
x86 and ARM. The resulting ratio cannot be attributed solely to the emulator.
The original benchmark assertions are retained; RegExp and NavierStokes lack
numerical result assertions, so successful completion is not an independent
correctness oracle for those cases.
