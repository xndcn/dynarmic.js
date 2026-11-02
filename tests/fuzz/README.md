# Differential fuzz corpus

The A32 and A64 campaigns compare the WASM JIT with Unicorn. A mismatch can be
saved as a stable JSON case instead of relying on the campaign seed:

```sh
python3 tests/a64/fuzz.py --host build-em/tests/a64_fuzz.js \
  --count 2000 --seed 7 --save-corpus tests/fuzz/corpus/a64
python3 tests/fuzz/replay.py tests/fuzz/corpus/a64/case-0000.json \
  --host build-em/tests/a64_fuzz.js
```

Cases use schema `dynarmic.js/fuzz-case-v1` and contain the exact guest words
or bytes, initial registers/vectors/flags, data page, memory mode and strict
comparison settings. Replay regenerates the Unicorn result and invokes the
host with one binary record. It exits nonzero for a mismatch, unsupported
status or host failure. Keep reduced cases under `tests/fuzz/corpus/`, with the
toolchain, Dynarmic revision and original command recorded in the change that
adds them. The checked-in corpus contains small A32 ARM/Thumb and A64
arithmetic/memory/SIMD cases. Verify all of them after an Emscripten build:

```sh
python3 tests/fuzz/check_corpus.py \
  --a32-host build-em/tests/a32_fuzz.js \
  --a64-host build-em/tests/a64_fuzz.js
```

The strict regression gate runs this command when Unicorn and Capstone are
installed. Generated campaign output should remain outside the source tree.

## Campaign coverage gates

`--count` must be positive and specifies the number of oracle-accepted cases
to generate. Random campaigns fail if the bounded generation loop cannot
produce that many cases. An explicit `--word` campaign retains its limited
attempt semantics and may accept fewer cases; accepting zero always fails.

Campaign summaries report `cases`, `compared`, `unsupported` and `mismatches`
separately. `compared = cases - unsupported`; zero actual comparisons always
fails, even when every host record is marked unsupported and no mismatch was
observed. Any mismatch also fails.

Use `--max-unsupported-rate FRACTION` to limit the fraction of oracle-accepted
cases that the host does not compare. The inclusive limit must be finite and
between 0 and 1; standalone campaigns default to 1. The regression gate passes
explicit limits for its fixed seeds: **A64 0.05, Thumb 0.10, A32 ARM 0.30**.
These are limits on un-compared cases, including self-modifying programs and
cases that do not reach the host's normal halt. They are not instruction-set
support percentages. The host's existing unsupported classification is
unchanged; do not increase these limits to conceal lost comparison coverage.

The orchestration and threshold tests need only Python's standard library:

```sh
python3 tests/fuzz/test_campaign.py -v
```

They execute both real campaign entry points against deterministic oracle and
host status streams, covering empty/partial generation, all-unsupported
results, inclusive thresholds, invalid arguments and mismatch propagation.
Unicorn differential execution remains a separate runtime gate.
