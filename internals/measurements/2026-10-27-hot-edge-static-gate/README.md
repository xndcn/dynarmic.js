# A64 focused-reselection static gate — 2026-10-27

## Status and decision boundary

**Static eligibility: PASS for a small, opt-in focused-reselection prototype.**
This is not approval to enable a policy by default, a CoreMark performance
result, or evidence of a speedup. No runtime implementation is included in this
static-gate milestone.

The preceding design review rejected immediate implementation: ordinary regions
`0x1202c` and `0x1237c` each already contain 64 blocks, so their union cannot fit
the unchanged limits. It allowed only this bounded static-eligibility phase.
The useful question is whether a small required closure can be prioritized while
leaving unrelated blocks external, not whether the two complete regions can be
merged.

The accepted backend and rejected register-cache-pruning experiment are
unchanged. No additional cache-pruning timing cohort is permitted.

## Inputs and identity

- Transfer-evidence source: `ec4701f20d2e53a133367e4048f3f0fc2b981c82`.
- Upstream dynarmic: `a41c380246d3d9f9874f0f792d234dc0cc17c180`.
- CoreMark workload: `ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa`.
- A64 flat binary SHA-256:
  `8f96e0f0f3beb9a5fb8765405926655c182bfcdfe93a8f99dc6ea32dec227ab1`.
- Exact dynamic evidence:
  [../2026-10-27-region-transfer-diagnostics/README.md](../2026-10-27-region-transfer-diagnostics/README.md).
- Unchanged selection limits: 64 blocks, 2,000 IR instructions, and 4,096 bytes
  of guest-PC distance.

The offline selector probe validates its reconstructed ordinary 64-block region
against the retained compile generation before analyzing a focused selection.
It does not execute the workload or emitted modules.

## Decode and semantic correction

The exact guest instructions are:

```text
0x12370: sub  w11, w11, #58
0x12374: cmn  w11, #11
0x12378: b.ls 0x12418
0x1237c: mov  w0, #4
```

Thus `0x12370` is a direct-branch block, **not a `BL`**. Optimized IR has six
instructions and terminal `If{ls, 0x12418, 0x1237c}`. There is no architectural
link write, `PushRSB`, or call continuation to inline. All further work must be
described as bounded direct-branch boundary elimination.

The standalone target region's 47,999,998 RSB returns must not be attributed to
this direct edge. They are considered only when deciding which actual return
blocks and return sites a selected region must preserve; they are not a claimed
call/return pair or a predicted saving.

## Static closure

The probe reconstructs optimized IR using the pinned flat binary and the same
`A64AddressSpace::GenerateIR` pipeline as the JIT.

| Property | Ordinary baseline | Focused model |
| --- | ---: | ---: |
| Selected blocks | 64 | 64 |
| IR instructions | 511 | 524 |
| Layout | fallback | fallback |
| Entry reachable | yes | yes |
| Hot `0x12370 -> 0x1237c` internal | no | yes |

The complete reachable graph within the guest-distance limit has 88 blocks and
701 IR instructions. The required closure has only 17 blocks and 127 IR
instructions:

```text
0x12020 0x1202c 0x121fc 0x1220c 0x12218 0x12220 0x12228 0x12238
0x122e0 0x12330 0x1233c 0x12370 0x1237c 0x123a4 0x12418 0x12424
0x12428
```

It contains the entry-to-hot-exit path and paths from the hot target to all
three dynamically observed nonzero RSB return blocks (`0x12418`, `0x12424`,
`0x12428`). There is no `BL` continuation requirement. Deterministic FIFO fill
of the remaining budget preserves every one of the seven existing return-site
descriptors.

Relative to the ordinary region, the filled candidate adds `0x1237c` and
`0x12418`, and displaces `0x12350` and `0x12398`. This is not a union of the two
original 64-block regions.

## Mechanism accounting, not performance accounting

The model internalizes two currently measured external direct edges:

| Edge | Exact taken count in each retained memory mode |
| --- | ---: |
| `0x12370 -> 0x1237c` | 47,199,999 |
| `0x12370 -> 0x12418` | 2,399,999 |
| **Total** | **49,599,998** |

The same focused selection makes three formerly internal edges external:

```text
0x122f8 -> 0x12398
0x1233c -> 0x12398
0x1233c -> 0x12350
```

Their previous dynamic counts are **unknown**, because internal edges were not
instrumented. They must not be reported as zero. The candidate also leaves
`0x1237c -> 0x12388` external. Consequently 49,599,998 is neither a net transfer
reduction nor a runtime saving. An actual prototype must measure all resulting
external exits and whole-Run totals before performance qualification.

## Offline emission sanity check

A separate native offline emission probe used the same sorted descriptors and
entry index 1, with no inline memory window and no chaining-slot allocation.
Those settings explain why these bytes are not comparable with the retained
20,996-byte diagnostic module.

| Result | Baseline | Focused model |
| --- | ---: | ---: |
| Module bytes | 17,551 | 19,408 |
| Entry body bytes | 17,342 | 19,199 |
| Native `EmitWASM` only, upper median of fixed 20 trials | 0.191290 ms | 0.198970 ms |
| Node/V8 `WebAssembly.compile`, upper median of fixed 20 trials | 0.085830 ms | 0.088302 ms |

Both modules passed `WebAssembly.validate`; their SHA-256 hashes were
`3e5a89aa815d658b85988cebbea49475f7ec4bde907c916c98098f877b29a306` and
`9effb79067b9a658cc6c94a872f8cb093fa134e94d7b4c8baab4e492b7405102`.
Each native trial performed IR translation before starting its timer, so that
column measures emission alone. These are static compile-cost observations only,
not benchmark cohorts and not runtime evidence. The candidate is larger, so no
code-size benefit is claimed.

## Retained evidence and reproduction

`evidence/analysis.json` is the complete reviewed selector output. It includes
all 88 reconstructed blocks and successor relations, exact required/final
selections, baseline-derived return sites, measured internalization and unknown
former-internal accounting, input hashes, load base and `block_dump` hash. All
38 fields present in the original scratch analysis match it exactly; the
permanent probe adds fail-closed identity, successor and return-site evidence.

`evidence/` also retains the original native emission result, V8 output, exact
scratch emitter source, both generated modules and their inventory hashes.
`emit_probe.cpp` is the portable native source and `compile_probe.mjs` is the
retained V8 helper. `evidence/commands.md` records exact commands, tool/source
identity checks, original/reproduced module hashes and the scope of the timing
fields. The original inline V8 helper was not retained; this limitation is
recorded rather than replaced with a fabricated provenance claim.

Run the focused tests, verify the frozen evidence and reproduce the selection
into a new output path:

```sh
node --test internals/measurements/2026-10-27-hot-edge-static-gate/probe.test.mjs \
  internals/measurements/2026-10-27-hot-edge-static-gate/verify.test.mjs
node internals/measurements/2026-10-27-hot-edge-static-gate/verify.mjs
node internals/measurements/2026-10-27-hot-edge-static-gate/probe.mjs \
  build-native/tests/tools/block_dump \
  /tmp/dynarmic-coremark-long/coremark_a64.bin 0x10000 \
  internals/measurements/2026-10-27-region-transfer-diagnostics/evidence/a64-flat-window.json.gz \
  /tmp/hot-edge-analysis.json
```

Expected test count is 29/29. The verifier checks the complete seven-file
inventory, independent frozen payload hashes and semantic constraints, including
coordinated tampering with a regenerated inventory. Its expected summary is:

```text
hot-edge static evidence integrity PASS files=7 full=88/701 final=64/524 internalized=49599998 returnSites=7; runtime/performance not assessed
```

The expected probe summary is:

```text
static probe PASS full=88/701 baseline=64/511 required=17/127 final=64/524
```

The portable native emitter reproduced both archived Wasm hashes exactly. Its
separate native integration tests require `EMIT_PROBE` and `EMIT_PROBE_INPUT`;
all 3/3 cover stale-output preservation, unwritable-output failure and fresh
byte-identical Wasm reproduction. These are static mechanism reproducibility
checks only; they do not qualify runtime behavior or performance.

## Prototype gates

Only after the reproducible static evidence is retained may an opt-in prototype
be implemented through TDD. It must:

1. Leave selection and generated bytes exactly unchanged when focus is absent
   or disabled, and leave all existing limits unchanged.
2. Admit the required entry/hot-target/return closure first, then fill remaining
   budget deterministically; leave every nonselected edge external.
3. Preserve guest ordering, `entry_index`, RSB behavior, invalidation,
   ownership, aliases and the other callers of the standalone target region.
4. Add synthetic selector fixtures and real generated-Wasm mechanism tests,
   including former-internal edges that become external and return behavior.
5. Compare exact whole-Run transfer histories, not just the chosen edge. Stop if
   comparable transfers are merely displaced or if compile cost becomes
   disproportionate.
6. Pass ordinary correctness and strict regression gates before any paired
   complete-duration CoreMark qualification. Diagnostic durations, isolated
   emission times, module sizes and transfer counts are not speedups.

A failed mechanism or performance gate means retaining the ordinary selector
and recording the rejection without widening budgets or selectively discarding
samples.
