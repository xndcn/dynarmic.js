# A64 live region-alias coverage — 2026-10-24

This record answers one bounded design question: when CoreMark later compiles an
entry that was selected in an earlier region, is a compatible earlier region
still live and does it actually expose that block as an emitted fallback return
site? It is compile-history evidence and a generated-Wasm equivalence screen,
**not a throughput result or production acceptance of multi-entry regions**.

## Decision

| Observation | Value |
| --- | ---: |
| Compiled regions | 111 |
| Structured / single / fallback layouts | 79 / 11 / 21 |
| Later entries previously selected elsewhere | 82 |
| Later entries with a live eligible owner | 16 |
| Pre-registered STOP threshold | 8 or fewer |
| Result | **GO for bounded prototype planning/TDD only** |

The 16 covered compilation generations are:

```text
22, 23, 26, 27, 28, 29, 33, 38, 42, 43, 44, 52, 53, 72, 80, 81
```

A later entry is covered only when all of the following hold for at least one
owner generation immediately before that compilation:

- both the later entry and owner use A64;
- the owner has fallback-dispatch layout;
- the descriptor is not the owner's original entry;
- the selected block was actually emitted and is an actual `PushRSB` return site;
- the owner's exact region-generation ID is in the sorted live-ID snapshot.

This is a per-entry Boolean coverage test, not a count of dynamic entries or a
prediction of saved compilations. Multiple eligible owners still cover an entry
only once. Invalidation makes an old generation ineligible even if its
append-only history remains available.

## v2 evidence and exact v1 projection

`dynarmic.js/region-diagnostics-v2` retains the complete v1 fields plus:

| Field | Meaning |
| --- | --- |
| `layout` | `single`, `structured` or `fallback` |
| `blocks[].descriptor` | Full 64-bit selected-block identity |
| `blocks[].emitted` | Actual emitter callback observation |
| `blocks[].entryReachable` | Reachability from the original entry in the planning CFG |
| `blocks[].returnSite` | Membership in the region's actual return-site set |
| `liveRegionIdsBeforeCompile` | Sorted unique IDs of live `BlockEntry` owners before this miss compiles |

The live IDs come from actual cache entries, not descriptor-based reconstruction.
Each successful instantiation receives the ID returned by the diagnostic
recorder. History remains append-only; live IDs describe ownership at one
compile boundary and do not make old snapshots live views.

The analyzer validates both complete CoreMark reports, recomputes native/guest
metrics and duplication, checks normal halt and matching CRCs, and requires the
ordered v2-to-v1 projection to deep-equal every field of the retained v1 history.
It also requires equal duplication attribution, unchanged workload and Dynarmic
upstream identity, 111 regions and 82 later entries. The projection matches
exactly; duplicate selections remain 3,315 and module bytes remain 708,917.

## Generated-Wasm comparison

Fresh production and diagnostic processes each produced 111 modules, halted
normally, and reported matching native/guest CoreMark CRC `0x4983`. Both compile
totals were 111 regions and 708,917 bytes.

All 111 module pairs have equal byte lengths and identical normalized WAT after
masking only classified embedded runtime addresses. There are zero
raw-byte-identical pairs; raw equality is neither expected nor claimed.

| Masked runtime-address context | Differences |
| --- | ---: |
| Flat-window memory offsets (`223236` versus `227076`) | 2,273 |
| Link-slot table indices | 278 |
| Link-slot miss addresses stored at `ExecutionContext + 8` | 278 |
| Paired RSB fast-dispatch entry addresses | 460 |
| Fast-dispatch table base addresses | 97 |
| Fast-dispatch miss addresses at `ExecutionContext + 12` | 0 |
| **Total** | **3,386** |

Every other WAT line and token must match. The comparator also checks ordered
link-slot load/miss pairing and fails on missing modules, size changes,
unclassified numeric changes and non-numeric changes. WABT 1.0.39 was built from
the official source after its published archive hash was verified because the
official prebuilt binary required a newer host glibc.

This comparison applies to these module pairs and explicit address contexts. It
is not a general Wasm semantic-equivalence proof.

## Provenance

- Base revision: `d21e6b6e6741b66600a915e7dcfa68eec3e929c3`, with the
  pre-commit diagnostic-v2 changes described by this record.
- Dynarmic submodule: `a41c380246d3d9f9874f0f792d234dc0cc17c180`.
- Release build, Emscripten 6.0.13, profile OFF, region diagnostics ON for the
  history; a separate production build supplied the comparison modules.
- CoreMark revision `1f483d5b8316753a742cbf5590caf5bd0a4e4777`, 200,000
  iterations, A64 architecture, flat-window memory mode 2.
- Workload-manifest SHA256:
  `ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa`.
- Native and guest CoreMark CRCs match. Diagnostic Run duration is deliberately
  excluded from the analysis and must not be used as a benchmark score.
- Full environment, artifact hashes, raw output and workload identity are in the
  compressed report. Exact commands are in `commands.md`.

## Files

- `a64-flat-v2.json.gz`: complete raw v2 CoreMark report and immutable history.
- `analysis.json`: complete projection/coverage analysis and all 82 entry rows.
- `generated-wasm.tar.gz`: `production/` and `diagnostics/`, each containing
  `block_1.wasm` through `block_111.wasm`.
- `generated-wasm-comparison.json`: per-module sizes, raw and normalized-WAT
  hashes, and classified runtime-address differences.
- `analyze.mjs`: fail-closed offline projection and coverage analyzer.
- `verify.mjs`: inventory, independent hashes, fresh analysis, cohort, generated
  module and archive verifier.
- `commands.md`: collection, comparison and reproduction commands.
- `SHA256SUMS`: hashes of the five retained data artifacts.
- `analyze.test.mjs`, `verify.test.mjs`: projection, threshold-boundary,
  invariant and coordinated-tamper regression coverage.

The raw v2 report, analysis, module archive and comparison report have
independent expected SHA-256 values in `verify.mjs`; these are checked before
any decompression, JSON analysis or archive extraction. Refreshing `SHA256SUMS`
cannot hide coordinated changes to those payloads. `commands.md` is the mutable
command log and is covered by the inventory.

## Reproduction and verification

From the repository root:

```sh
node --test tests/bench/region_alias_coverage.test.mjs \
  internals/measurements/2026-10-24-region-alias-coverage/analyze.test.mjs \
  internals/measurements/2026-10-24-region-alias-coverage/verify.test.mjs
node internals/measurements/2026-10-24-region-alias-coverage/verify.mjs
```

Expected verifier output:

```text
region-alias evidence GO regions=111 laterEntries=82 covered=16 files=5
```

To independently regenerate the normalized-WAT comparison, extract the retained
archive into an empty temporary directory and run:

```sh
node internals/measurements/2026-10-24-region-diagnostics/compare_generated_wasm.mjs \
  /tmp/region-alias-wasm/production /tmp/region-alias-wasm/diagnostics \
  /path/to/wabt/bin/wasm2wat /tmp/region-alias-comparison.json
```

The fresh JSON must deep-equal `generated-wasm-comparison.json`, including the
WABT version. The earlier retained evidence verifier also exercises ordinary
instruction and inconsistent link-slot mutation rejection with both `wasm2wat`
and `wat2wasm`.

## Interpretation and next boundary

The 16/82 result passes only the pre-registered evidence gate for planning and
TDD of the smallest potentially sound multi-entry prototype. It does not
establish dynamic transfer frequency, saved bytes, compile-latency improvement,
throughput improvement or production eligibility.

The prototype remains A64-only, fallback-dispatch-only and return-site-only. It
requires explicit masked-`state.pc` initial selection, fail-closed unknown-PC
behavior, the existing two-argument ABI, one shared allocation with
full-descriptor aliases, no overwrite of live owners, all-alias invalidation and
exactly-once table/link release. External-callee RSB return must pop exactly
once. Structured regions and single-step behavior stay unchanged. Direct-link,
RSB, fast-dispatch, unknown-PC, destruction and invalidation tests must precede
any performance evaluation.

Do not broaden this result to A32, Thumb, structured regions, other memory modes
or applications. Do not add generated-code counters unless a concrete hot-edge
decision remains unresolved, resume global block-cap sweeps, or treat the
552,378-byte whole-region attribution as a promised saving.
