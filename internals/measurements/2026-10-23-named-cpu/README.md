# Named compiler CPU diagnostics — 2026-10-23

Base implementation: `c1a33c9201093daf07bebfb398bdfca29837d01b`.
No backend or guest implementation was changed for this experiment. The dirty
checkout during collection contained the CPU analyzer, its tests and CTest
registration. The separate Release build used `--profiling-funcs` and
`DYNARMIC_JS_PROFILE=OFF`; executable hashes, configured flags and source
observations are in the archive. These observations do not attest build-time
source identity.

Node v24.19.0 / V8 13.6.233.17-node.51, Emscripten 6.0.13, Linux x86_64,
Core i7-12700. Affinity was unrestricted `0-19`, frequency was not locked.
Collection ran sequentially, without concurrent builds/tests launched by this
session. Sampling requested 100 microseconds; observed median deltas were
153–155 microseconds. The largest cohort gap was 10,753 microseconds.
This is diagnostic evidence, not an optimization comparison or a claim of
equivalent performance across hosts/engines.

## Findings and next experiment

Thirty fresh processes cover five rotations of six cases: A32, Thumb and A64
fixed memory kernels in flat-window mode; 4,096 A32 entries in thunk mode;
and complete A32/A64 4,000-iteration CoreMark in flat-window mode. The latter
are short correctness workloads, not duration-qualified scores.

| Workload | A64 decode-table subtree / all samples | A64 decode-table subtree / compiler samples |
| --- | --- | --- |
| A64 fixed | 40.5–47.0% | 54.9–59.5% |
| A64 CoreMark | 8.2–9.4% | 41.3–43.0% |

“Compiler samples” selects `AddressSpace::GetOrEmit` and descendants, including
engine compilation/instantiation calls. Detached GC samples are excluded from
that subtree. Percentages are unweighted observations, not measured time or
an upper bound on achievable speedup. Each range covers all five processes.

The source corroborates the A64 hotspot:
`frontend/A64/decoder/a64.h::GetDecodeTable` constructs 643 matchers, stable-sorts
by specificity, stable-partitions three prioritized SIMD rules, then scans the
list for all 4,096 buckets and copies matching `std::function` wrappers.
The named `GetDecodeTable<TranslatorVisitor>` subtree remains prominent in both
the fixed and application workload. This selects **constant A64 decode buckets
and static dispatch** as the next experiment. It has not been implemented here.

Required semantic protections for that experiment:

- Keep upstream `a64.inc`, argument extraction and 12-bit projection.
- Preserve the stable specificity ordering **and** the priority exceptions for
  `MOVI, MVNI, ORR, BIC (vector, immediate)`, `FMOV (vector, immediate)` and
  `Unallocated SIMD modified immediate`. ARM's ordering alone is insufficient.
- Compare every ordered bucket against the unmodified upstream runtime table;
  exercise every rule, bit mutations, seeded random words and typed dispatch.
- Run sanitizer, A64 differential fuzz, both profiling configurations and
  browser correctness gates. Collect compatible alternating Release fixed,
  population and CoreMark reports before accepting a speedup. The latest A64
  instruction-count correction changes workload identity: older fixed reports
  must not be silently compared with `c1a33c9` hosts.

For A32 population, `Module` plus `Instance` self samples comprise 50.9–53.7%
of the compiler subtree. Emission-phase medians are 59.66 ms versus 15.51 ms
translation across 4,096 entries, while the fixed A32 median is 10.93 ms emission
versus 10.04 ms translation. These instrumented totals motivate subsequent
module/emission work, but do not justify changing region policy yet. Large
`std::function` or destructor-named samples may be inlined/merged code; inspect
their callers and generated code before attributing cost to a particular source
operation. The generic sample analyzer deliberately does not guess categories
from those names.

## Reproduce and inspect

Configure the separate build using the [CPU recipe](../../../docs/Performance.md), then
build `jit_bench jit_thumb_bench jit_a64_bench jit_cachepopbench arm_jit`.
Use a validated external CoreMark workload from its existing build protocol.
The retained collector requires a new output directory:

```sh
python3 internals/measurements/2026-10-23-named-cpu/collect.py \
  /tmp/new-cpu-cohort /tmp/dynarmic-named-cpu /tmp/dynarmic-coremark-v1-final
node tests/bench/cpu_profile.mjs /tmp/new-cpu-cohort/a64-0.cpuprofile \
  --root 'AddressSpace::GetOrEmit' > /tmp/a64-compiler.json
```

The collector retains exact argv, exit status, stdout/stderr, raw profiles,
summaries and pre/post executable hashes. It rejects existing output directories
and failed child processes. It is an experiment recipe, not a replacement for
the benchmark protocols' workload validation. The original complete cohort was
additionally validated against those protocols, including CoreMark's known
CRCs and a fresh native reference invocation.

`evidence.tar.gz` contains the complete cohort, derived subtree summaries and
observations, native CoreMark output, four initial exploratory profiles, build
and test logs, the original collector and per-file SHA256 inventory. To audit
the saved profiles and workload outputs without rerunning the JIT:

```sh
mkdir /tmp/named-cpu-audit
tar -xzf internals/measurements/2026-10-23-named-cpu/evidence.tar.gz -C /tmp/named-cpu-audit
node internals/measurements/2026-10-23-named-cpu/verify.mjs /tmp/named-cpu-audit
```

The verifier recomputes all cohort summaries, checks raw profile hashes,
subtree counts, sample completeness, fixed/population results and CoreMark CRCs
against the saved native output. It does not rerun native or JIT executables,
prove source provenance, or qualify performance claims. All 30 profiles and
workload outputs pass. Node protocol tests pass 178/178 (19 new CPU analyzer
tests), and native CTest passes 4/4 after rebuilding/reconfiguring. Backend code
is unchanged; the full strict regression was not rerun for this analysis-only
milestone. Release and performance-leadership gates remain open.
