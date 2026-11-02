# Exact A64 region-transfer evidence — 2026-10-27

Source: `ec4701f20d2e53a133367e4048f3f0fc2b981c82`; upstream dynarmic:
`a41c380246d3d9f9874f0f792d234dc0cc17c180`. Both reports record a clean
checkout. The diagnostic host was Release, region diagnostics ON and CPU
profiling OFF. The workload is the pinned 200,000-iteration CoreMark build,
identity `ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa`.
Both Runs halted normally with native/guest CRC `0x4983`.

This is exact control-transfer mechanism evidence, **not a benchmark score or
speedup**. The instrumentation adds diagnostic work, and retained `runMs` fields
are not performance comparisons.

## Whole-Run counts

A64 flat-window and page-table modes produced identical dynamic counts:

| Counter | Exact decimal count |
| --- | ---: |
| Compiled regions | 111 |
| Generated-module entries | 298,200,235 |
| Taken external transfers | 298,200,064 |
| `external-direct-call` | 145,800,018 |
| `rsb-return` | 152,400,030 |
| `other-tail-transfer` | 16 |

Entries minus external transfers is 171. This arithmetic is retained for
reconciliation, but 171 is not relabeled as an independently measured dispatcher
count. The Run-only conservation identity was separately executed in the host
tests; it is not applied to Step.

`external-direct-call` counts every taken direct chained edge, not only an
architectural call instruction. A last-target field is only the latest dynamic
observation, not a target histogram.

## Hot-region attribution

| Region / edge | Direct | RSB | Other |
| --- | ---: | ---: | ---: |
| `0x10b00` (`iterate`) | 199,998 | 0 | 0 |
| `0x10b18` (`iterate`) | 199,997 | 0 | 0 |
| `0x1202c` (`core_bench_state`) | 88,899,996 | 0 | 0 |
| `0x1237c` | 0 | 47,999,998 | 0 |

The largest single direct edge in the retained history is exit block `0x12370`
in caller region `0x1202c`, statically targeting `0x1237c`, with **47,199,999**
taken transfers in each mode. The target region has 48,000,000 entries and
47,999,998 RSB returns. These observations make this caller/callee boundary a
concrete candidate for further cost evaluation; they do not quantify its cost,
prove that inlining is beneficial, or establish a per-target RSB histogram.

Consequences for the next experiment:

- Do not prioritize fast-dispatch elimination for this workload: only 16 taken
  transfers used `other-tail-transfer`.
- Do not motivate an RSB optimization from the two hot `iterate` entries: their
  RSB and other-tail counts are zero.
- Evaluate callsite/callee boundary elimination for `0x12370 -> 0x1237c` before
  considering a broader rewrite. Preserve the 64-block cap and qualify any
  candidate with correctness, generated-byte, compile-time and paired
  complete-duration gates. The identical flat/page counts show that this control
  mechanism is shared across those memory modes; they do not predict a speedup.

No runtime optimization is included in this evidence milestone, and no rejected
cache-pruning timing cohort was repeated.

## Evidence and reproduction

`evidence/` contains both compressed raw reports, their exact collection record,
a derived `analysis.json`, and `SHA256SUMS`. The raw compressed bytes are pinned
in `verify.mjs`. Verification recomputes CoreMark parsing and CRC identity,
source/workload/environment binding, compile and transfer attribution, strict
transfer analysis, hot-edge extraction, top-region summaries and cross-mode
count equality. It does not execute or compare benchmark timings.

```sh
node internals/measurements/2026-10-27-region-transfer-diagnostics/verify.mjs
node --test internals/measurements/2026-10-27-region-transfer-diagnostics/verify.test.mjs
```

Expected verifier output:

```text
region transfer evidence PASS cases=2 transfers=298200064 hotEdge=47199999,47199999
```

The four tests additionally prove that stale derived analysis fails even with a
refreshed inventory, modified raw bytes fail the frozen hash, and dirty-source
tampering fails semantic validation. To regenerate the analysis from new raw
reports, use a new output path:

```sh
node internals/measurements/2026-10-27-region-transfer-diagnostics/analyze.mjs \
  <flat-report.json[.gz]> <page-report.json[.gz]> <new-analysis.json>
```
