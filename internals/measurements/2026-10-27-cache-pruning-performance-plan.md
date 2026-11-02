# Structured register-cache pruning performance qualification plan

Registered: 2026-10-27, after the semantic RED/GREEN test and before collecting
any baseline/candidate timing samples for this change.

## Implementations and workload

- Baseline: `b0aa3d1bf1e0a75af81c3bdd064a7204c3266c39` (default A64 fallback
  alias publication disabled).
- Candidate: `8c91fbe` backend code. It filters register-cache collection to
  original-entry-reachable blocks only after structured planning succeeds.
- Workload: pinned CoreMark `1f483d5b8316753a742cbf5590caf5bd0a4e4777`,
  200,000 iterations, 2K performance seeds. Reuse only after validating the
  retained workload identity
  `ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa`.
- Both implementations use independent Release, profile-OFF Emscripten builds
  from clean source archives. No generated-code execution counters or dumping
  are enabled. The diagnostic mechanism builds are separate and are never used
  for timing.

## Static and semantic mechanism gate

Before paired timing, collect complete 200,000-iteration A32 and A64 flat-window
region-diagnostic reports for both implementations. These are compile-history
observations, not benchmark scores. Require all of the following:

1. Normal guest termination and native/guest CRC equality in every report.
2. Identical region counts, entry descriptors, selected descriptors, layouts,
   and per-block emitted/entry-reachable/return-site masks. Cache pruning must
   not change selection or control-flow planning.
3. No per-region module-byte or entry-body-byte growth. Single-block and
   fallback module sizes must be unchanged. Only structured regions with
   omitted blocks may shrink.
4. At least one structured region with an omitted block must shrink in the A64
   report; otherwise the intended mechanism is not exercised and timing is not
   authorized. Retained earlier A64 CoreMark history contains many such regions,
   but this is not a substitute for observing the candidate.
5. Preserve both raw reports and a comparison containing every differing region.
   A failed gate stops the experiment and retains the evidence.

The existing generated-Wasm smoke test independently requires fallback alias
cache coverage and byte-identical structured output when an omitted block's
register usage changes. Native/Emscripten tests and strict regression have
already passed at the candidate revision.

## Timing protocol and acceptance

1. Run one ordinary six-case 4,000-iteration CoreMark smoke report per
   implementation. Check schemas, normal halts and native/guest CRCs. These
   short observations are correctness screens only; neither their timings nor
   their comparisons qualify performance.
2. If the mechanism and smoke gates hold, run five fresh processes per
   implementation for every A32/A64 × memory-mode case at 200,000 iterations,
   alternating implementation order and rotating cases. Every guest sample must
   last at least ten seconds. Use affinity `0,2,4,6`, empty `NODE_OPTIONS`, no
   concurrent builds/tests, and retain every observation and failure.
3. Apply the unchanged CoreMark comparator: candidate/baseline median ratios
   must not exceed 1.0526315789473684 for kernel time, 1.15 for Run or compile
   time, or 1.10 for emitted bytes. Inspect MAD and raw samples; exclude no
   samples and widen no budgets.
4. Accept the optimization only with correct execution and passing gates. Call
   it a speedup only for measured kernel-time improvement; byte reduction alone
   is not a speedup. A failed result requires disabling/redesigning the change
   or a separately explained noise follow-up, while preserving the failure.

After qualification, collect fresh Run-scoped profiles of the accepted default
configuration before selecting another runtime optimization. Do not re-enable
A64 fallback aliases or broaden this cache mask to fallback compilation.
