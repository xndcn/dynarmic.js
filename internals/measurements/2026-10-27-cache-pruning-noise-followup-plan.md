# Cache-pruning complete-duration noise follow-up plan

Registered: 2026-10-27 after the original complete-duration comparison failed and
before collecting any replacement timing samples. This is a separately registered
noise investigation, not a reinterpretation of the original result.

## Preserved original failure

The original experiment used the already registered
[performance protocol](2026-10-27-cache-pruning-performance-plan.md), baseline
`b0aa3d1bf1e0a75af81c3bdd064a7204c3266c39`, candidate `8c91fbe`, and the pinned
200,000-iteration CoreMark workload. It completed all 60 fresh-process guest
samples with normal halts, matching native/guest CRCs and no exclusions. Every
sample exceeded ten seconds; the shortest was 23.000 seconds.

The original comparison **failed**. Its sole failed budget was A32 thunk compile
time: baseline median 84.700447 ms, candidate median 143.708962 ms, ratio
1.6966730057438149 against the unchanged 1.15 limit. The original report remains
a failure regardless of this follow-up. No sample will be dropped and no budget
will be widened.

## Evidence for a noise investigation

The candidate's A32 thunk compile totals were 80.045, 207.980, 143.709, 293.041
and 74.141 ms. The baseline's were 73.109, 366.163, 86.185, 83.431 and 84.700 ms.
The candidate's elevated samples show correlated increases across translation,
optimization, emission, V8 compilation, instantiation and table publication,
including phases untouched by register-cache pruning. Large multi-phase stalls
also occur elsewhere in both implementations, alongside large guest-kernel
outliers. This supports investigating host scheduling/frequency noise; it does
not prove that the original failure was noise and is not itself an acceptance
result.

The candidate's first four A32 thunk processes compiled the same 118 blocks as
all five baseline processes. Its fifth compiled 116 blocks, matching the already
documented A32 history variability. Raw compilation histories therefore remain
part of the follow-up analysis; no equal-history assumption is introduced.

## Follow-up protocol

1. Keep both source revisions, independent production builds, workload, Node/V8,
   compiler settings and artifact identities unchanged from the original
   experiment. Diagnostic/profile builds remain excluded from timing.
2. Collect a new independent cohort with the exact original collector: five fresh
   processes per implementation and case, alternating implementation order and
   rotating cases, affinity `0,2,4,6`, empty `NODE_OPTIONS`, and no concurrent
   builds/tests. All six cases and every observation are mandatory; every guest
   sample must last at least ten seconds.
3. Run the unchanged comparator on the new five-process cohort. Separately pool
   all five original and all five follow-up samples per implementation/case,
   remap only the derived follow-up process indices to 5–9, recompute summaries,
   and run the same comparator on all ten observations. Raw reports stay
   untouched; the pooled reports are derived evidence.
4. Accept the unchanged cache-pruning candidate only if **both** the independent
   follow-up cohort and the ten-sample pooled comparison pass every original
   kernel, Run, compile and emitted-byte budget with correct execution. If either
   comparison fails, disable or redesign the change. Do not run repeated cohorts
   until a passing one appears, replace failed samples, or select favorable
   cases.
5. Report both follow-up and pooled results alongside the original failure.
   Kernel-time improvement may be called a speedup only when directly measured;
   byte reduction alone is not a speedup.

After acceptance, collect fresh Run-scoped profiles of the accepted default
configuration before selecting another runtime optimization. After rejection,
preserve the evidence and return to the last accepted default before further
performance work.
