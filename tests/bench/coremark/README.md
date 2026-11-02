# CoreMark workload protocol

This optional workload exercises A32 and A64 in thunk, page-table and
flat-window modes. CoreMark source is external. The default 4,000-iteration
run is a **correctness smoke**, not a publishable CoreMark score.

Requirements: a configured browser build, Node, Python, `arm-none-eabi-gcc`,
`arm-none-eabi-objcopy`, `python3 -m ziglang`, a host C compiler and Git.
Build with one clean upstream revision and retain the workload directory:

```sh
git clone https://github.com/eembc/coremark.git /tmp/coremark-source
git -C /tmp/coremark-source checkout --detach 1f483d5b8316753a742cbf5590caf5bd0a4e4777
python3 tests/bench/coremark/build.py /tmp/coremark-source /tmp/coremark-short --iterations 4000
node tests/bench/coremark/run.mjs build-em/examples/browser /tmp/coremark-short \
  --runs 5 --output coremark-short.json
node tests/bench/coremark/validate.mjs coremark-short.json
```

The convenience wrapper keeps its artifacts even on failure:

```sh
COREMARK_OUT=/tmp/coremark-new tests/bench/coremark/run.sh /tmp/coremark-source build-em 4000 --runs 1
```

`COREMARK_OUT` must not contain previous build outputs. The wrapper defaults
to a retained temporary directory and five processes per case. `ARM_JIT` is
no longer accepted: select a configured build tree so flags and profile mode
can be recorded. Collectors refuse existing report/failure paths.

For the duration gate, build a separate workload with enough fixed iterations
for **every guest sample** to last at least ten seconds. 200,000 iterations
worked on the development machine; faster machines may need more:

```sh
python3 tests/bench/coremark/build.py /tmp/coremark-source /tmp/coremark-long --iterations 200000
node tests/bench/coremark/run.mjs build-em/examples/browser /tmp/coremark-long \
  --runs 5 --require-duration --output coremark-long.json
node tests/bench/coremark/validate.mjs coremark-long.json --require-duration
node tests/bench/coremark/compare.mjs before.json after.json --require-duration
```

Comparison requires five fresh processes per case and compatible workload,
host/runtime, build configuration and upstream Dynarmic revision. Budgets:
5% kernel-throughput loss, 15% Run/compile latency increase, 10% emitted-byte
or profile cache-peak increase. A comparison without `--require-duration`
can diagnose smoke runs but does not qualify them as performance scores.
Use controlled CPU affinity, alternating experiments and inspect dispersion;
this collector rotates case order but does not alternate two implementations.

The native executable is run once as an independent CRC reference, including
the iteration-dependent final CRC. It need not last ten seconds and its speed
is not compared with the JIT. Known 2K performance-seed CRCs are also checked.
Only the exact upstream short-duration error is permitted for smoke runs;
other errors, abnormal process exits and guest fault halts fail collection.
The browser host uses `UserDefined1` for semihost exit (including its existing
unknown-SVC exit behavior) and `UserDefined2` for exceptions/interpreter fallback.

Each sample creates a new process, module and JIT and calls Run once. Run wall
time excludes module loading, guest RAM setup and JIT construction; it includes
cold compilation and statistics publication. The kernel timer covers upstream
`iterate`, including any JIT misses and engine tiering within that interval.
This is not a steady-state or engine-tier-completion claim. A 32-bit microsecond
timer bounds sample duration; collection timeout defaults to 300 seconds and
is capped at one hour. No native/guest compiler equivalence is assumed.

For a diagnostic V8 CPU profile of exactly one Run, invoke `profile.mjs` with the
same four sample arguments plus a new `.cpuprofile` path. It retains the ordinary
sample record, enables generated guest-PC names, and preserves the profile on
guest failure. This is sampling instrumentation, not a benchmark collector or a
duration-qualified comparison. See [the profiling protocol](../../../docs/Performance.md#cpu-sampling-and-generated-code-review)
for the complete command and analyzer usage.

`coremark-build-v1` records upstream revision, consumed-source hashes, the port
and builder hashes, compiler versions, flags, full argv and executable hashes.
`coremark-workload-v1` records those inputs, JIT artifact hashes, configured
build/source and observed host identities, raw guest output, parsed metrics,
compile phases, optional profile counters, medians and MADs. Source identities
are checkout observations, not proof that a stale JIT artifact was built from
that checkout. Do not rebuild during collection. Workload identity includes all
nested manifest inputs except path-dependent command argv.

On child failure, `<report>.failure.json` retains child status, stdout/stderr
and completed samples. A valid short report is retained even when
`--require-duration` rejects it. Preflight errors occur before child execution
and are reported on stderr. `durationQualified` means the duration and
correctness checks passed; it is **not CoreMark certification**. Follow upstream
README, LICENSE and reporting rules before publishing scores or using marks.

To include this external workload in regression, pass it explicitly:

```sh
tests/regress.sh --strict --coremark /tmp/coremark-short build-em build-native
```

An absent requested workload or failed collection fails that gate. Without the
option, regression reports that the application gate was not covered. CI fetches
the pinned revision and runs a one-process short correctness check in both
profile configurations, retaining reports; it is not a performance gate.
