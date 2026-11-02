# Focused region whole-Run transfer evidence

This package compares one unfocused baseline Run with one exact-focus candidate
Run of the same A64 CoreMark workload. It is **mechanism evidence only**. It is
not a CoreMark score, paired duration experiment or speedup claim.

## Provenance and correctness

- Collection source: `b0a98606a748d6e4ba9f64bffaa30f823438846b`, clean for both Runs.
- Upstream dynarmic: `a41c380246d3d9f9874f0f792d234dc0cc17c180`.
- Workload identity: `ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa`.
- Architecture/memory mode: A64, numeric mode `0` (callbacks/thunks).
- Baseline focus: explicitly empty.
- Candidate focus: `0x000000000001202c,0x0000000000012370,0x000000000001237c,0x0000000000012418,0x0000000000012424,0x0000000000012428`.
- Both reports have diagnostics and profiling enabled, exactly one Run, zero
  Steps, normal halt and matching native/guest CRC `0x4983`.
- Both complete histories contain 111 compile generations and 111 aligned
  transfer generations. Region IDs are generation ordinals, not cross-report
  identities.
- The baseline and candidate JS artifacts are identical; their Wasm artifacts
  differ. Exact artifact hashes are pinned by the verifier and recorded in
  [commands.md](evidence/commands.md).

The analyzer reparses CoreMark output, rebuilds every derived report field from
raw leaves, checks compile/transfer attribution and independently recomputes:

```text
sum(module entries) = dispatchCount + sum(external transfers)
```

Both reports satisfy the identity with dispatch count 171:

| Run | Module entries | External transfers | Conservation |
|---|---:|---:|---|
| Baseline | 298,200,235 | 298,200,064 | Pass |
| Candidate | 232,600,235 | 232,600,064 | Pass |

## Whole-Run mechanism totals

The following are complete taken-external-transfer totals, not per-edge
estimates and not timing results:

| Kind | Baseline | Candidate | Candidate - baseline |
|---|---:|---:|---:|
| External direct calls | 145,800,018 | 113,000,018 | -32,800,000 |
| RSB returns | 152,400,030 | 119,600,030 | -32,800,000 |
| Other tail transfers | 16 | 16 | 0 |
| **All external transfers** | **298,200,064** | **232,600,064** | **-65,600,000** |

The candidate has 65,600,000 fewer taken external transfers over the complete
Run. This whole-Run comparison is valid because both conserved reports measure
the same class of external transitions over the same workload. It does not
assign zero dynamic executions to baseline-internal edges and does not
reconstruct unavailable per-edge baseline counts.

## Complete generation and edge history

Canonical generation identity retains full descriptors, entry index, layout,
ordered block membership/emission/reachability/return-site metadata and module
sizes, excluding only unstable region/live IDs. Edge identity additionally
retains the full exit descriptor, transfer kind and static target. Dynamic
last-target fields are observations, not target histograms.

- Generations: 108 unchanged, 3 removed, 3 added.
- Changed entry histories: 5, including the focused entry, two removed entries
  and two replacement entries.
- Exits: 439 unchanged, 29 removed, 33 added.
- Zero-count exits retained: 21 removed, 25 added and 376 unchanged with zero
  on both sides.
- Duplicate generation or edge identities and ambiguous provenance fail closed.

The focused candidate generation emits the direct source `0x12370`, target
`0x1237c` and all three required return blocks, all entry-reachable. The
baseline external direct edge `0x12370 -> 0x1237c` has 47,199,999 transfers;
that corresponding external exit is absent from the focused candidate
generation. The candidate also internalizes the baseline direct edge from
`0x12370` to `0x12418`, which has 2,399,999 transfers.

Three newly externalized candidate direct edges are explicitly attributed:

| Source | Target | Candidate count | Baseline-internal count |
|---|---|---:|---|
| `0x122f8` | `0x12398` | 27,999,999 | Unknown / not measured |
| `0x1233c` | `0x12398` | 4,799,999 | Unknown / not measured |
| `0x1233c` | `0x12350` | 4,799,999 | Unknown / not measured |

Their candidate total is 37,599,997. These rows remain in the complete added
edge union; they are not hidden to make the mechanism look better. Mixed or
ambiguous baseline internal/external provenance is rejected rather than
resolved by guessing.

The unchanged standalone target generation `0x1237c` records 47,199,999 fewer
external RSB returns across its three return sites, matching the hot-edge count
as mechanism accounting. The complete raw and derived histories, including all
other changes, are retained in `evidence/analysis.json`; no top-N filter is
used.

Compile-count amplification did not occur in this pair: both profiles contain
111 compiled blocks/modules. Profile `regionBlocks` changes from 4,166 to 4,168
and `emittedBytes` from 677,583 to 680,103, an increase of 2,520 bytes. These
are compile mechanism observations, not evidence of runtime improvement.

## Analyzer correction preserved

The first analyzer invocation failed closed with:

```text
Cannot analyze focused region transfer: missing focused return descriptors: 0x0000000000012418
```

The analyzer had incorrectly required `returnSite: true`. Real focused return
blocks are present, emitted and entry-reachable with `returnSite: false`.
The synthetic fixture and oracle were corrected through TDD; the unchanged raw
reports then passed. No failed result was discarded and the first invocation
created no analysis output. See [commands.md](evidence/commands.md).

## Verification

Run the synthetic protocol/tamper suite and the real frozen-evidence verifier:

```sh
node --test \
  internals/measurements/2026-10-27-focused-region-transfer/analyze.test.mjs \
  internals/measurements/2026-10-27-focused-region-transfer/compare.test.mjs \
  internals/measurements/2026-10-27-focused-region-transfer/verify.test.mjs \
  internals/measurements/2026-10-27-focused-region-transfer/verify_real.test.mjs

node internals/measurements/2026-10-27-focused-region-transfer/verify.mjs
```

The verifier checks independent frozen-byte pins, the exact inventory, fresh
raw-history analysis and semantic expectations for provenance, artifacts,
partition counts, whole-Run totals and newly externalized edges. Synthetic
mutations cover refreshed inventories, coordinated raw/derived/analysis
changes, unsafe paths, duplicate identities and removed zero-count rows.

No performance qualification has been performed by this package. Any later
paired complete-duration experiment must be a separately documented gate.
