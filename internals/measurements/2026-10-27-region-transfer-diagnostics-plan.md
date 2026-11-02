# Dynamic region-transfer diagnostics plan — 2026-10-27

## Purpose and boundary

The restored-default CPU profiles identify hot A64 regions but cannot separate
useful guest work from external control-transfer overhead. Add exact dynamic
counts before selecting another runtime optimization. This change is diagnostic
infrastructure only: it must not change production generated Wasm, production
snapshot shape, region selection, the 64-block cap, register flushes, or memory
semantics. Diagnostic observations are not benchmark scores or speedups.

Instrumentation is compiled only with `DYNARMIC_JS_REGION_DIAGNOSTICS`. It uses
no JS callback, new Wasm import/global/table, or production runtime branch.
Counters and metadata are owned by each JIT's append-only diagnostic history.
Invalidation and module release do not erase historical observations;
recompilation receives a fresh region ID and fresh zeroed counters.

## Event definitions

Count only a taken external `return_call_indirect` from a generated module:

| Published kind | Emission site |
| --- | --- |
| `external-direct-call` | Successful linked branch of `EmitChainedLink` |
| `rsb-return` | Matching, linked, no-halt branch of `PopRSBHint` |
| `other-tail-transfer` | Successful hit branch of `EmitFastDispatch` |

An RSB miss that subsequently succeeds through fast dispatch is only an
`other-tail-transfer`. Do not count direct-link misses, RSB index zero or
mismatch, halt-pending paths, dispatcher returns, internal region edges,
internal region returns, single-step exits, ordinary returns, or host/helper
calls. A module-entry counter records every generated-module entry, including
fallback aliases.

Counters are exact wrapping `i64` values in the shared linear-memory address
space used by generated modules. Records have stable addresses. Each external
site increments its counter immediately before the taken tail call and retains
the last dynamic descriptor and PC. Direct sites retain their static target as
the dynamic target. The last-target fields are not a per-target histogram.

## Publication

Keep `snapshot.regionDiagnostics` and schema
`dynarmic.js/region-diagnostics-v2` unchanged. Add only in diagnostic builds:

```text
snapshot.regionTransferDiagnostics
schema = dynarmic.js/region-transfer-diagnostics-v1
```

Each region publishes `regionId`, `frontend`, `callerEntryDescriptor`,
`callerEntryPc`, exact hexadecimal `entryCount`, and an `exits` array. Each exit
publishes region-local numeric `exitSite`, hexadecimal `exitBlockDescriptor`,
`kind`, nullable hexadecimal `staticTargetDescriptor`, exact hexadecimal
`count`, `lastDynamicTargetDescriptor`, and `lastDynamicTargetPc`. PCs and
descriptors are fixed-width 64-bit strings; counters are never JavaScript
Numbers. Before its first taken transfer, an exit's last-target fields are
zero. Snapshots and nested arrays/objects are frozen; previously returned
leaves are never mutated.

Publication occurs at Run/Step boundaries independently of the compile-history
publication cache, because dynamic counts can change without compilation.
Alternating JITs must not mix ownership or rewrite held snapshots.

The possible conservation identity

```text
sum(module entries) = dispatchCount + sum(external transfers)
```

is a hypothesis to test, not an assumed contract. Dispatcher-return reasons and
Step semantics must be checked before using it as evidence validation.

## TDD and verification

1. RED: native metadata/history tests and JS strict schema/projection tests;
   diagnostic-host publication absence/presence, exact values above 2^53,
   immutability, per-instance ownership, and fixed-fixture production
   generated-byte equivalence.
2. Implement stable records and the separate serializer; keep existing v2
   consumers unchanged.
3. RED: execute real generated Wasm fixtures for direct hit/miss/halt, RSB
   hit/mismatch/index zero, fast-dispatch hit/miss, internal edges/returns,
   single-step, aliases, invalidation, and recompilation.
4. Implement entry and taken-site instrumentation only in diagnostic builds;
   rerun all execution and publication tests.
5. Add CoreMark sample/report plumbing with workload identity and strict
   validation. Collect A64 flat-window and page-table diagnostic reports only
   after correctness gates pass.
6. Run native and Emscripten rebuilds, native CTest, all JavaScript protocol
   tests, diagnostic and diagnostic-plus-profile hosts, and strict production
   regression. Request independent code review immediately after implementation.

Do not run another cache-pruning timing cohort. Do not choose or implement a
runtime optimization until the resulting exact counts identify a concrete
eligible mechanism; counts alone still do not quantify its cost or establish a
speedup.
