# CoreMark workload baseline — 2026-10-22

This is a correctness/protocol milestone, not an implementation speedup or a
certified CoreMark score. The old script returned success even when upstream
reported insufficient duration and `Errors detected`; the new collector checks
process status, CRCs and duration separately. Production/profile browser hosts
also distinguish unhandled guest faults from normal semihost exits.

Environment: Intel i7-12700, Linux, Node v24.19.0, affinity `0-19`, Release with
profile OFF/ON. Runtime base revision: `08fb914`, with the browser halt change
in `browser-host.patch.gz`; upstream Dynarmic was unchanged. Full identities,
compiler flags, port/builder/source hashes and artifact hashes are in each raw
report. Builds and tests were stopped during timed collection. CPU frequency
was not controlled and affinity was unrestricted, so this is diagnostic data.

`production-short.json.gz` and `profile-short.json.gz` retain five independent
processes for each of six cases, using 4,000 fixed iterations and the same
workload binaries. Every stage and final CRC matches the native reference.
All samples are below ten seconds. Production kernel observations:

| Case | Kernel median (ms) | MAD (ms) | Run median (ms) | Compile median (ms) | Emitted bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| A32 thunk | 1407.733 | 14.151 | 1487.103 | 85.255 | 737512 |
| A32 page table | 703.040 | 23.373 | 786.928 | 92.749 | 876114 |
| A32 flat window | 535.155 | 9.929 | 614.233 | 84.139 | 822958 |
| A64 thunk | 1299.939 | 27.523 | 1435.873 | 130.430 | 641247 |
| A64 page table | 671.481 | 6.026 | 803.395 | 145.987 | 755630 |
| A64 flat window | 509.695 | 10.522 | 639.366 | 135.160 | 697150 |

Profile observations are retained independently, not used for an overhead
claim. They have exact cache peaks and identical emitted-byte totals to the
production cases. Run excludes module load/setup/JIT construction; kernel time
includes compilation misses and engine tiering. Native speed is not compared.

Reproduction (see the [protocol](../../../tests/bench/coremark/README.md) for
source checkout and tool requirements):

```sh
python3 tests/bench/coremark/build.py /tmp/dynarmic-coremark-source /tmp/dynarmic-coremark-v1-final --iterations 4000
node tests/bench/coremark/run.mjs build-em/examples/browser /tmp/dynarmic-coremark-v1-final --runs 5 --output /tmp/coremark-production.json
node tests/bench/coremark/run.mjs build-em-profile/examples/browser /tmp/dynarmic-coremark-v1-final --runs 5 --output /tmp/coremark-profile.json
python3 tests/bench/coremark/build.py /tmp/dynarmic-coremark-source /tmp/dynarmic-coremark-long --iterations 200000
node tests/bench/coremark/run.mjs build-em/examples/browser /tmp/dynarmic-coremark-long --runs 1 --require-duration --output /tmp/coremark-duration.json
```

Use fresh output paths; the builder and collector refuse overwrites. Upstream
CoreMark revision is `1f483d5b8316753a742cbf5590caf5bd0a4e4777`. No upstream
source or executable is included in this archive. The adaptation changes only
the documented UART stub in a build-tree copy; the repository supplies the
platform timer, types, seeds and freestanding support.

`production-duration.json.gz` retains one process per case at 200,000 iterations.
Kernel times were A32 thunk/page-table/flat-window 67.770/31.808/24.442 seconds
and A64 60.394/30.645/22.361 seconds. All six passed the duration gate and
native CRC comparison. One process is sufficient to exercise this gate but
insufficient for the comparator's five-process minimum; these observations
must not support an implementation comparison. `short-duration-rejection.log`
retains the expected rejection of the five-process short production report.
No failed optimization measurement was discarded or turned into a speedup.

Verification logs retain the 159 protocol tests, native CTest 4/4, full strict
regression with the external workload, and Chrome 133 / Firefox Worker checks
for both profile configurations. Browser timings in these smoke logs are
correctness diagnostics; those checks ran alongside differential fuzzing and
are not controlled performance observations. The convenience `run.sh` wrapper
also completed a fresh build and one-process six-case collection; attempting
to reuse that build output was rejected with exit status 2. The comparator's
self-comparison passed; synthetic tests exercise regressions and incompatible
experiments. No baseline/candidate implementation comparison is claimed here.

`sources.sha256` identifies the finalized collector/validator and browser host
sources. Compressed reports retain the observations gathered before the final
parser review added the score-summary/extra-line check; all archived reports
also pass the final validator. CI configuration is included, but hosted CI was
not run locally.

Full regression commands (both ended with `regression done fail=0`):

```sh
tests/regress.sh --strict --coremark /tmp/dynarmic-coremark-v1-final build-em build-native
tests/regress.sh --strict --coremark /tmp/dynarmic-coremark-v1-final build-em-profile build-native
```

The corresponding additional correctness reports remain at
`/tmp/tmp.NnWMqnjlnY/report.json` and `/tmp/tmp.Ubvzn0nupe/report.json` locally;
these runs are not substituted for the separately archived baseline cohorts.
