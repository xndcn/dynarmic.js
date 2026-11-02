# A64 fallback alias static qualification — 2026-10-24

**Decision: STOP before performance timing.** The preregistered mechanism gate
expected exactly 95 candidate compilation regions: 111 baseline regions minus
the 16 later entries classified as having a live eligible fallback return-site
owner. The candidate compiled 99. This experiment is therefore inconclusive for
performance and is not an accepted throughput or compile-latency result.

The bounded alias implementation had already passed its separate semantic
qualification at `6a6c6d7`. This record does not find a semantic defect and does
not promote that implementation to performance-validated status. No short paired
screen, complete-duration CoreMark comparison, browser benchmark or release
claim was started after the static failure.

## Frozen comparison

Baseline revision `f96fe7a0b4bc7a098ef9badf64919930e2cf8a71`; candidate revision
`6a6c6d79b2c21d6590ebda65848d8883f94f8a31`; both used Dynarmic
`a41c380246d3d9f9874f0f792d234dc0cc17c180`. Independent Release Emscripten
6.0.13 diagnostic builds used profile OFF, region diagnostics ON and the same
CoreMark workload: revision `1f483d5b8316753a742cbf5590caf5bd0a4e4777`,
200,000 iterations, A64 flat-window mode 2. Both guests halted normally and
matched the native CRC `0x4983`. Artifact and workload identities are retained
in the reports and `commands.md`.

| Static observation | Baseline | Candidate | Delta |
| --- | ---: | ---: | ---: |
| Compiled regions | 111 | 99 | -12 |
| Selected blocks | 4,177 | 3,767 | -410 |
| Duplicate selections | 3,315 | 2,901 | -414 |
| Generated module bytes | 708,917 | 576,602 | -132,315 |
| Exported entry-body bytes | 694,277 | 564,056 | -130,221 |

The 18.66% generated-byte reduction is a static observation, not a substitute
for the failed exact-region gate and not an engine-memory or timing result.
Diagnostic phase and Run times in the raw reports are deliberately excluded.

## Exact discrepancy diagnosis

The earlier coverage classifier remains correct for its documented purpose: a
Boolean per-entry test for the existence of a live eligible owner in the
baseline. It explicitly was not a candidate compile-history predictor. The
performance plan erred by treating its 16 covered rows as 16 guaranteed net
compilation removals.

The candidate history decomposes without residue:

```text
111 - 15 alias-reused entries - 5 non-alias absorbed entries + 8 new entries = 99
99 - 95 = 1 owner-dependency survivor - 5 non-alias absorbed entries + 8 new entries = 4
```

1. **Fifteen predicted entries were actually alias-reused.** Baseline region IDs
   22, 26, 27, 28, 29, 33, 38, 42, 43, 44, 52, 53, 72, 80 and 81 have no
   candidate original-entry compilation and are exposed by an actual candidate
   fallback return-site alias.
2. **One predicted entry survives because its predicted owner is itself
   absorbed.** Descriptor `0x0000000000010380` was baseline region 23 and becomes
   candidate region 22. Its only baseline eligible owner was region 22, original
   descriptor `0x0000000000010370`; that owner has no candidate original-entry
   compilation. No candidate region exposes `0x10380` as an alias. Baseline
   coverage was real, but assuming that owner would still compile independently
   in the candidate was invalid.
3. **Five additional baseline entries disappear without aliases.** Baseline
   regions 30, 34, 39, 40 and 41 (`0x1135c`, `0x1238c`, `0x123f8`, `0x1229c` and
   `0x12174`) are emitted as ordinary interior blocks in candidate regions.
   They are not return-site alias reuse and cannot be credited to the registered
   16-row coverage model.
4. **Eight new original entries compile.** Candidate regions 25, 26, 27, 30, 33,
   35, 36 and 37 introduce `0x11698`, `0x11980`, `0x1125c`, `0x12398`, `0x12350`,
   `0x12144`, `0x10098` and `0x10518`. Their descriptors were selected in baseline
   regions but were not baseline original entries. This is changed candidate
   region/transfer history, which the baseline-only linear deduction cannot
   predict.

The analyzer also checks every candidate original-entry miss against aliases
exposed by its exact live owner generations. It finds zero live-alias conflicts.
The surviving predicted entry and all eight new entries have no candidate alias
anywhere in the history. Thus the initial publication-conflict hypothesis does
not explain the discrepancy. No cache no-overwrite rule was weakened to make the
counts fit.

## Interpretation and next boundary

A sound next performance predictor would need candidate transfer/region-history
semantics, including dependencies between eliminated owners and newly exposed
entries. The retained compile-boundary baseline does not contain that dynamic
information. Do not repair the model by changing 95 to 99 after observation,
discarding the new cohort, or proceeding directly to timing. Do not add
production generated-code counters merely to force this static model to close.

Any renewed alias performance experiment needs a separately registered mechanism
gate that accounts for these effects, followed by fresh semantic checks and the
unchanged paired short and complete-duration budgets. Neutral, noisy or
inconclusive results do not establish an optimization. Independent callback/API,
bounded-cache/engine-memory, browser-performance and installable web packaging
release gates remain open.

## Evidence and verification

- `before-a64-flat.json.gz`, `after-a64-flat.json.gz`: complete raw diagnostic
  reports, including retained stdout and full region-generation histories.
- `candidate.patch`: source and test changes from baseline to candidate; it
  applies cleanly to the isolated baseline clone.
- `analysis.json`: fresh static totals, gate verdict and complete categorized
  entry sets from `analyze.mjs`.
- `commands.md`: build/collection identities and reproduction commands.
- `alias_replay.mjs`: reproduces original-first, no-overwrite publication and
  excludes single-step regions, matching the candidate's alias eligibility.
- `analyze.mjs`: validates environment, workload and stdout compiler identity,
  normal halt, native/guest CRC, duplication and compile totals before deriving
  the exact mechanism.
- `verify.mjs`: checks inventory and independently pinned payload hashes, then
  requires fresh analysis to deep-equal the retained analysis and the registered
  failure diagnosis.
- `*.test.mjs`: real-evidence, synthetic-mechanism, fail-closed and coordinated
  tamper tests.

From the repository root:

```sh
node --test internals/measurements/2026-10-24-region-alias-static-gate/*.test.mjs
node internals/measurements/2026-10-24-region-alias-static-gate/verify.mjs
```

Expected verifier output:

```text
region-alias static gate STOP baseline=111 expected=95 actual=99 aliasReused=15 added=8 files=5
```

`STOP` is the expected experiment verdict; verifier success means only that the
retained evidence independently reproduces that verdict. It is not performance
acceptance. Local hashes and source observations are not independent build-time
provenance attestation.
