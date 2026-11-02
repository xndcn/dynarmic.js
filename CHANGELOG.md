# Changelog

Changes intended for the first public release are collected here. The project
has not assigned a stable API or published a tagged release yet.

## Unreleased

- A private npm tooling entry point for the Pages CMake build, verified source
  packaging and Vite preview at the repository subpath, including real 404
  recovery. Artifact checks reject broken links, stale cache keys and mismatched
  engine/source hashes; Pages Actions are SHA-pinned.

- An experimental `dynarmic.js@0.1.0-alpha.0` npm candidate with a typed ESM
  factory, ARM32/Thumb/AArch64 CPU access, explicit Wasm asset overrides,
  cooperative execution and disposal. Tarballs include corresponding recursive
  source, licenses and integrity manifests, with isolated consumer validation.
- ARM32, Thumb and AArch64 translation to WebAssembly, with Node and browser
  Worker hosts.
- Callback, page-table and bounded flat-memory paths; explicit code-cache
  invalidation and per-instance statistics.
- Native IR tests, seeded Unicorn differential fuzzing, browser checks, and
  versioned correctness and performance reports.
- Paired comparisons with upstream native Dynarmic using identical ARM32,
  Thumb and AArch64 guest kernels, raw samples and build provenance.
- Independent Unicorn instruction-count checks for benchmark throughput;
  corrected the A32 memory loop count and verified full memory writeback.
- CTest and sanitizer coverage for all emitted-IR suites and installed-package
  consumers; fail-closed browser benchmark reports and collectors.
- Retained regression evidence with exact commands, separate guest/fuzz logs
  and replayable mismatch cases; CI uploads evidence on success and failure.
- Differential campaigns reject empty inputs, zero comparisons and excessive
  un-compared cases, with explicit coverage accounting and tested limits.
- Public build and embedding documentation, contribution and security policies,
  and an explicit `internals/` home for development notes and experiment archives.
- Browser guest-memory bounds checks and input validation, with recoverable
  fetch, Worker and execution failures.
- A minimal Node.js example and responsive browser demo with three memory modes,
  native-reference results, accessible status, cancellation and bounded execution.
- Complete browser build targets, strict guest-toolchain preflight, WebAssembly
  capability checks and lifecycle regression coverage.
- A project landing page with an interactive architecture guide, command
  copying, responsive dark/light layouts, accessible controls and social assets.
- Pages validation without automatic deployment, including real Chromium and
  Firefox checks, recursive corresponding-source bundles and publication gates.
- A checksum-pinned QuickJS example built for ARM32 and direct WebAssembly,
  with identical JavaScript LCG, sieve and π workloads in a three-runtime browser
  comparison, checked outputs, repeated timings and downloadable measurements.
- The complete V8 v7 JavaScript benchmark suite from checksum-pinned QuickJS
  extras: eight suites and ten selectable cases, upstream timing/checks,
  separate large-memory host, cancellation and exportable partial results.

See [the roadmap](docs/Roadmap.md) for limitations and future work. Performance
observations depend on the workload and engine; this release makes no comparative
performance claim.
