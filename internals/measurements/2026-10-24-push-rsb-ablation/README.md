# Diagnostic `PushRSB` continuation ablation — 2026-10-24

Base: `9731e9a779755edb3e61ca17ed8ea3798119f3ec`; upstream Dynarmic:
`a41c380246d3d9f9874f0f792d234dc0cc17c180`. This record preserves a
**diagnostic-only, workload-specific selector ablation**. The candidate is not
production policy, was not applied to the main tree, and is not an accepted
optimization. No timing comparison was run.

## Control-flow question

`RegionSelector::Successors` adds condition-failed and terminal successors and
then every `PushRSB` return location. A direct A64 `BL` therefore permits a
caller, callee and return continuation to be selected into one region. Removing
such an edge can add a real external return transfer; it is not universally
dead-code deletion.

The current module has one exported two-argument function for its originally
requested entry. Other selected descriptors are not registered alternate
entries. If an external callee returns to a continuation, selecting that
continuation only inside the caller's module does not let the callee enter it.
A link, fast-dispatch or RSB transfer carries a table index, not an interior
block index.

The probe suppresses exactly 28 full A64 return descriptors only when discovered
through `PushRSB`. Ordinary branches, condition-failed edges and direct entries
to the same values remain eligible. The fixed set is a causal experiment around
repeated `guest_main` continuations, not a hardcoded policy suitable for other
programs.

## Method

An isolated clone and separate baseline/candidate build trees were used. Both
are Release Emscripten builds with profile counters OFF and region diagnostics
ON. The complete 200,000-iteration A64 flat-window CoreMark workload supplied
one normal, native-CRC-validated compilation history per implementation.

The regenerated clean baseline's complete `regionDiagnostics` and `duplication`
objects matched the earlier archived baseline, not merely its aggregate counts.
The synthetic selector test first failed because all 28 speculative appends were
still present, then passed after only that append site was filtered. Coverage
includes exact candidate accounting, a high-bit descriptor alias, ordinary CFG
edges and direct entries. Native CTest passed 5/5 and the Emscripten diagnostic
host passed. Exact commands and outputs are in `commands.md`.

The comparator pins source/upstream revisions, workload and A64 executable
hashes, 200,000 iterations, architecture/memory mode and baseline totals. It
recomputes native/guest metrics from retained stdout, recomputes duplication and
compile totals, and requires a changed Wasm artifact. Identical JS is valid for
this C++-only change. Every target must occur in the baseline; candidate targets
may legitimately disappear. History matching ignores only region IDs, while
retaining their actual values in output.

## Static screening result

| Metric | Baseline | Candidate | Delta |
| --- | ---: | ---: | ---: |
| Compiled regions / entries | 111 | 111 | 0 |
| Selected block instances | 4,177 | 3,208 | -969 (-23.20%) |
| Unique descriptors | 862 | 855 | -7 |
| Descriptors selected more than once | 587 | 533 | -54 |
| Duplicate selections | 3,315 | 2,353 | -962 (-29.02%) |
| Generated module bytes | 708,917 | 690,520 | **-18,397 (-2.60%)** |
| Exported entry-body bytes | 694,277 | 676,174 | **-18,103 (-2.61%)** |
| Selections of the 28 targets | 451 | 25 | -426 |
| Duplicate selections of the 28 targets | 423 | 2 | -421 |

The strict static screen passes because both byte totals are lower. Five target
descriptors disappear completely; the other 23 remain reachable through entries
or ordinary control flow. This confirms edge-only filtering rather than address
blacklisting.

Region IDs and the entry-descriptor sequence are unchanged. Thirty-six entry
histories changed: 30 modules became smaller, two became larger and four kept
the same byte sizes. The increases are 577 bytes at `0x10ba8` and 64 bytes at
`0x10dd0`; the aggregate reduction does not hide them. Six retained hot entry
histories (`0x10b00`, `0x10b18`, `0x11700`, `0x11fd0`, `0x1202c`, `0x1209c`) are
exactly unchanged, including selected descriptors and both byte counts.

The 28-edge ablation shrinks parts of the region cascade but eliminates no
entry compilation. It therefore does not solve the underlying case where a
descriptor selected inside an earlier region is later compiled as a new entry.
The earlier 552,378-byte whole-region attribution is not a recoverable-byte
estimate and is not contradicted by this smaller result.

## Decision and next work

**Retain production behavior; archive the probe as static evidence only.** Do
not generalize the fixed descriptor set, claim a dynamic transfer improvement,
or start a duration-qualified performance cohort for this workload-specific
candidate. Candidate full differential fuzzing, sanitizers, browser runs and
paired performance collection were not required after this limited screen and
were not performed.

Read-only architecture review found live-region reuse semantically feasible but
direct aliasing to the existing export unsound. The smallest possible follow-up
is bounded to A64 fallback-dispatch regions and eligible return sites, selecting
the initial block from masked `state.pc` with a fail-closed unknown-PC path. It
would preserve the existing ABI and one table slot, but needs one shared region
owner with lightweight aliases and all-alias invalidation. Structured regions
must remain unchanged in that experiment.

Before implementation, classify the 82 later entries that had appeared in
earlier selections by layout, actual emission/reachability, return-site
eligibility and live-cache overlap. Compile history alone does not prove that
an earlier region was live or that a structured selected block was emitted. Add
compile-time eligibility/alias-hit diagnostics for the bounded prototype only;
do not add dynamic generated-code counters. Stop if eligible live overlap is
negligible.

## Files and verification

- `baseline.json.gz`, `candidate.json.gz`: complete raw reports and histories.
- `comparison.json`: exact verified static comparison, including all target and
  changed/hot entry histories.
- `candidate.patch`, `candidate-status.txt`, `targets.txt`: frozen isolated
  selector/test change and provenance.
- `commands.md`: clone, build, RED/GREEN, collection and comparison commands.
- `analyze.mjs`, `analyze.test.mjs`: pinned fail-closed comparator and 31 tests.
- `verify.mjs`, `verify.test.mjs`: fresh evidence audit and 16 tests, including
  coordinated mutations with deliberately refreshed inventory hashes and negative
  coverage of entry/region identity, static totals and changed/hot histories.
- `SHA256SUMS`: hashes of the seven retained data/provenance artifacts.

From the repository root:

```sh
node --test internals/measurements/2026-10-24-push-rsb-ablation/analyze.test.mjs
node --test internals/measurements/2026-10-24-push-rsb-ablation/verify.test.mjs
node internals/measurements/2026-10-24-push-rsb-ablation/verify.mjs
```

Expected verifier output:

```text
push-RSB ablation evidence PASS regions=111 changedEntries=36 hotEntries=6 files=7
```

The verifier checks inventory hashes, the exact patch paths/target set, native
and guest output, a fresh comparison equal to the retained JSON, entry identity,
target attribution and unchanged hot histories. Independent expected SHA-256
values for all seven evidence files prevent a refreshed inventory from blessing
coordinated patch-body, raw-report or same-count history tampering. Raw reports
were compressed with `gzip -9 -n`. Local hashes and source observations are
provenance records, not independent build attestation. The archived comparison's
PASS is only the static byte screen; it is not a correctness, throughput or
release verdict.
