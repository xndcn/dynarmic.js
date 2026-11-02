# One C workload, three browser execution paths

The Pages C lab compiles the same five kernels to ARM32 for dynarmic.js, x86
for v86, and direct WebAssembly. All three use the same Zig-provided Clang
frontend, O2 and scalar settings: vectorization, loop unrolling, fast-math and
floating-point contraction are disabled. There is no libc, allocation or I/O
inside a timed kernel. Each path has 16 MiB of guest or Wasm data memory.

Build the standalone artifacts from the repository root with Python 3.10+ and
the `ziglang` Python package (locally exercised with Zig 0.17.0 / Clang 22.1.8):

```sh
python3 examples/c-bench/build.py \
  --dist build-c-bench/dist --cache build-c-bench/cache
```

The complete page builds these assets through `browser_page`; see the
[browser guide](../browser/README.md). The standalone command leaves the
existing page distribution unchanged. The build also copies the checksum-pinned
official v86 npm release `0.5.473+g315b2b0`, its Wasm engine and license. Subsequent
builds reuse the verified archive. `c-bench-manifest.json` records the exact
compiler binary, common and target flags, commands, input hashes and artifacts.
The distributed `c-bench-v86-LICENSE.txt` combines v86's npm BSD license with
the complete notices for its bundled SoftFloat, Zstandard (including FSE,
Huff0 and xxHash), and MIT-licensed QEMU floppy code. The retained
[third-party notices](licenses/v86-third-party.txt) identify the pinned source
URLs and hashes; their own hash is included in the build manifest.

## Work and verification

Every case uses seed `0x12345678`. Counts below are kernel iterations per call;
they remain identical across all three paths.

| Case | Quick | Full | Timed work |
| --- | ---: | ---: | --- |
| Integer | 4,000,000 | 16,000,000 | Dependent uint32 multiply, XOR and rotate |
| Branch | 1,000,000 | 4,000,000 | Seeded data-dependent choices between distinct non-inlined callees |
| Stream | 8,000,000 | 32,000,000 | Sequential read, update and accumulation over 8,192 words |
| Pointer | 4,000,000 | 16,000,000 | Follow one shuffled 8,192-entry cycle with dependent loads |
| Float | 4,000,000 | 16,000,000 | Seeded binary64 dyadic products and sums |

`bench_init(caseIndex, iterations, seed)` resets input outside timing.
`bench_run()` performs the work and returns its uint32 checksum. Each guest
executes six identical calls: one first call, two discarded warmups and three
measured samples. The page can repeat this in three fresh Worker rounds. JIT
state is retained between calls inside a Worker; a first call may include guest
compilation and is reported separately.

[`reference.py`](reference.py) independently computes expected checksums with
Python integers. For the floating-point case it uses rational integer
numerators, so the oracle does not reuse host floating-point calculations.
The selected dyadic values and accepted iteration limit make all C binary64
products and sums exact. Every call, including warmups, must match the fixed
checksum in `c-bench-reference.json`.

The branch kernel has distinct `noinline` callees to retain conditional control
flow. Generated code was inspected for actual ARM/x86 conditional jumps and
calls, and VFP/SSE2/scalar-Wasm double arithmetic. The build retains compiler
assembly in its cache for further inspection.

## Host protocol

ARM loads the flat binary at `0x10000`; x86 loads a Multiboot ELF at `0x100000`.
Both use stack top `0xff0000` and a mailbox of little-endian 32-bit words at
`0xf000`:

| Words | Meaning |
| --- | --- |
| 0–3 | Request: magic `0x43424e31`, case index 0–4, iteration count, seed |
| 4 | Status: 1 running, 2 complete, 3 invalid request |
| 5 | Number of completed calls, 6 on success |
| 6–11 | Checksums in call order |

ARM records start/end using SVC 3, yielding twelve timestamps, then exits with
SVC 0 and R0 = 0. x86 writes phase 0/1 to port `0xE200` and exit status to
`0xE208`. The host timestamps these markers; input preparation stays outside
them. Direct Wasm exports only `memory`, `bench_init` and `bench_run`, has no
imports, and uses fixed 16 MiB memory without growth. Its host timestamps each
`bench_run` call.

These compare complete execution paths. ARM and x86 instructions, ABI,
floating-point instructions, memory access paths, JIT strategy and scheduling
differ, even with one compiler frontend and shared scalar C. Marker/call
overhead also differs. Preserve the browser, inputs, per-call samples and
per-round results when interpreting ratios; do not attribute a measured ratio
solely to an emulator. Earlier small-count calibration reports are development
evidence, not results for the final profiles above.

## Collect performance measurements

Use `measure.mjs` to execute the ordinary Extended Pages run in five separate
Chromium processes, retaining all raw reports and any failed sessions:

```sh
node examples/c-bench/measure.mjs --browser /path/to/chrome \
  --dist build-em/examples/browser/dist --sessions 5 --output NEW_RESULTS
```

Build first and keep the page files fixed during measurement. The collector
rejects diagnostic/profile hosts and verifies inputs before and after each
session. It does not silently retry or average away failed sessions. Summarize
paired round ratios within each session before combining session medians;
inner measured calls are not independent browser repetitions.

The [recorded five-session comparison](../../internals/measurements/2026-10-31-c-bench-performance/README.md)
contains the analysis script, raw evidence, per-case ratios, first-call and
preparation times, and the separately retained incomplete Chrome 133 attempt.
