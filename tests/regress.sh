#!/bin/bash
# Regression gate for the wasm backend.
#
#   tests/regress.sh [--strict] [--coremark WORKLOAD_DIR] [EM_BUILD] [NATIVE_BUILD]
#       (defaults: build-em build-native)
#
# EM_BUILD     Emscripten build tree (JIT hosts: jit_runbin.js, smc_test.js, ...)
# NATIVE_BUILD host build tree (ARM guests, reference programs, IR-level tests)
#
# Every guest runs under jit_runbin in memory modes 0 (thunks), 1 (page table
# fastmem) and 2 (flat window), must reach its PASS marker, and the *_log
# variants must reproduce the reference program's mix log exactly.
# Set DYNARMIC_JS_TEST_ARTIFACTS to a new directory to retain every command,
# guest result and fuzz reproducer on success as well as failure.
set -uo pipefail
INVOCATION=("$0" "$@")
INVOCATION_CWD="$PWD"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
strict=0
if [ "${1:-}" = "--strict" ]; then
    strict=1
    shift
fi
coremark=""
if [ "${1:-}" = "--coremark" ]; then
    coremark="$(cd "${2:?--coremark needs a built workload directory}" && pwd)" || exit 2
    shift 2
fi
if [ "$#" -gt 2 ] || [[ "${1:-}" == --* ]] || [[ "${2:-}" == --* ]]; then
    echo 'Usage: tests/regress.sh [--strict] [--coremark WORKLOAD_DIR] [EM_BUILD] [NATIVE_BUILD]' >&2
    exit 2
fi
EM="$(cd "${1:-$ROOT/build-em}" && pwd)" || exit 2
NATIVE="$(cd "${2:-$ROOT/build-native}" && pwd)" || exit 2
HOSTS="$EM/tests"
GUESTS="$NATIVE/tests/guest"
keep=0
if [ -n "${DYNARMIC_JS_TEST_ARTIFACTS:-}" ]; then
    # Atomic creation refuses existing directories and symlinks: an earlier
    # successful or failed run must never be overwritten.
    if ! mkdir -- "$DYNARMIC_JS_TEST_ARTIFACTS"; then
        echo "Artifact directory must be new: $DYNARMIC_JS_TEST_ARTIFACTS" >&2
        exit 2
    fi
    WORK="$(cd "$DYNARMIC_JS_TEST_ARTIFACTS" && pwd)" || exit 2
    keep=1
else
    WORK="$(mktemp -d)" || exit 2
fi
cleanup() {
    status=$?
    printf 'exit_code=%s\n' "$status" >> "$WORK/run.txt"
    if [ "$status" -eq 0 ] && [ "$keep" -eq 0 ]; then
        rm -rf "$WORK"
    else
        echo "Regression artifacts: $WORK" >&2
    fi
}
trap cleanup EXIT
mkdir -p "$WORK/commands" "$WORK/guests" "$WORK/fuzz" || exit 2
{
    printf 'command='; printf '%q ' "${INVOCATION[@]}"; printf '\n'
    printf 'invocation_cwd=%s\nsource_root=%s\n' "$INVOCATION_CWD" "$ROOT"
    printf 'strict=%s\nem_build=%s\nnative_build=%s\ncoremark_workload=%s\n' "$strict" "$EM" "$NATIVE" "$coremark"
    printf 'started='; date -u +%FT%TZ
    printf 'revision='; git -C "$ROOT" rev-parse HEAD 2>/dev/null || true
    printf 'upstream='; git -C "$ROOT/externals/dynarmic" rev-parse HEAD 2>/dev/null || true
} > "$WORK/run.txt"
cd "$WORK"

fail=0
bad() { echo "FAIL $*"; printf '%s\n' "$*" >> "$WORK/failures.txt"; fail=1; }
skip() { echo "SKIP $*"; printf '%s\n' "$*" >> "$WORK/skips.txt"; }

# Preserve stdout and stderr separately before forwarding them to existing
# parsers. Even commands whose output is suppressed below retain full evidence.
# mktemp keeps invocations distinct when functions execute in a pipeline.
logged() {
    local entry status
    entry="$(mktemp -d "$WORK/commands/command.XXXXXX")" || return 1
    printf '%q ' "$@" > "$entry/command.sh"
    printf '\n' >> "$entry/command.sh"
    printf '%s\n' "$PWD" > "$entry/cwd.txt"
    command "$@" > "$entry/stdout.log" 2> "$entry/stderr.log"
    status=$?
    printf '%s\n' "$status" > "$entry/exit-code.txt"
    cat "$entry/stdout.log"
    cat "$entry/stderr.log" >&2
    return "$status"
}
node() { logged node "$@"; }
python3() { logged python3 "$@"; }

fuzz() {
    local name="$1"
    shift
    python3 "$@" --save-corpus "$WORK/fuzz/$name.corpus" > "$WORK/fuzz/$name.log" 2>&1 \
        || { bad "$name"; head -20 "$WORK/fuzz/$name.log"; }
}

# Small fixed-program hosts.
node "$HOSTS/jit_demo.js" | grep '^PASS' >/dev/null || bad jit_demo
node --test "$ROOT"/tests/bench/*.test.mjs >/dev/null || bad benchmark_protocol
DYNARMIC_JS_BROWSER_DIST="$EM/examples/browser/dist" node --test "$ROOT"/examples/browser/*.test.mjs "$ROOT"/examples/quickjs/*.test.mjs "$ROOT"/examples/quickjs/v86/*.test.mjs >/dev/null || bad browser_input_and_failures
python3 -m unittest discover -s "$ROOT/tests/browser" -p '*_test.py' >/dev/null || bad browser_source_package
benchmark_runs=1
[ "$strict" -eq 1 ] && benchmark_runs=5
node "$ROOT/tests/bench/run.mjs" "$HOSTS" --runs "$benchmark_runs" --output benchmark.json >/dev/null \
    || bad benchmark_runner
node "$ROOT/tests/bench/validate.mjs" benchmark.json >/dev/null \
    || bad benchmark_schema
node "$HOSTS/smc_test.js" "$GUESTS/smc.bin" | grep '^PASS' >/dev/null || bad smc_test

node "$HOSTS/thumb_it_return_test.js" | grep '^PASS' >/dev/null || bad thumb_it_return_test
node "$HOSTS/instance_test.js" | grep '^PASS' >/dev/null || bad instance_test
node "$HOSTS/region_diagnostics_test.js" | grep '^PASS' >/dev/null || bad region_diagnostics_test
node "$HOSTS/a64_region_alias_test.js" | grep '^PASS' >/dev/null || bad a64_region_alias_test
node "$HOSTS/a64_region_sharing_test.js" | grep '^PASS' >/dev/null || bad a64_region_sharing_test
node "$HOSTS/a32_region_alias_test.js" | grep '^PASS' >/dev/null || bad a32_region_alias_test
node "$HOSTS/runtime_table_test.js" | grep '^PASS' >/dev/null || bad runtime_table_test
node "$HOSTS/stats_test.js" | grep '^PASS' >/dev/null || bad stats_test
node "$HOSTS/lifecycle_test.js" | grep '^PASS' >/dev/null || bad lifecycle_test
node "$ROOT/tests/bench/cache_run.mjs" "$HOSTS" --runs "$benchmark_runs" --output cache-benchmark.json \
    || bad cache_benchmark_runner
node "$ROOT/tests/bench/cache_validate.mjs" cache-benchmark.json >/dev/null \
    || bad cache_benchmark_schema
node "$ROOT/tests/bench/cache_population_run.mjs" "$HOSTS" --runs "$benchmark_runs" --output cache-population.json \
    || bad cache_population_runner
node "$ROOT/tests/bench/cache_population_validate.mjs" cache-population.json >/dev/null \
    || bad cache_population_schema

# Planner properties and decoder equivalence (checks stay enabled in Release).
logged "$NATIVE/tests/region_plan_test" || bad region_plan
logged "$NATIVE/tests/arm_decoder_test" || bad arm_decoder

# IR-level tests (emitted natively, executed by Node).
logged "$NATIVE/tests/smoke_ir" smoke.wasm >/dev/null 2>&1 && node "$ROOT/tests/ir/smoke.mjs" smoke.wasm >/dev/null || bad smoke_ir
logged "$NATIVE/tests/sat64_ir" sat64.wasm >/dev/null && node "$ROOT/tests/ir/sat64.mjs" sat64.wasm >/dev/null || bad sat64_ir
logged "$NATIVE/tests/wasm_binary_test" . >/dev/null && node "$ROOT/tests/ir/wasm_binary_test.mjs" . >/dev/null || bad wasm_binary_test
for test in integer_edge_test vector_edge_test fp_edge_test memory_edge_test; do
    node "$ROOT/tests/ir/$test.mjs" "$NATIVE/tests/$test" || bad "$test"
done

# Frontend-only and full-pipeline translation of the same program, run by the JS dispatcher.
for mode in --raw ""; do
    specs=()
    dispatch_dir="dispatch-${mode#--}"
    [ "$dispatch_dir" != dispatch- ] || dispatch_dir=dispatch-optimized
    mkdir "$dispatch_dir" || { bad "$dispatch_dir"; continue; }
    for a in 1000 1008 1010 1014; do
        logged "$NATIVE/tests/tools/block_dump" $mode loop $a "$dispatch_dir/b$a.wasm" >/dev/null 2>&1 || bad "block_dump${mode:+ $mode} $a"
        specs+=("$a=$dispatch_dir/b$a.wasm")
    done
    node "$ROOT/tests/tools/dispatch.mjs" "${specs[@]}" 1000 2000 \
        --expect halt=0,r0=0,r1=16,r15=0x2000,z=1 >/dev/null 2>&1 || bad "dispatch loop${mode:+ $mode}"
done

# Guests run to completion.
for t in coproc crc crypto fp16 fp media neon neon2 neon3 neon4 neon5 neon6 qc test; do
    for m in 0 1 2; do
        run_log="guests/a32-$t-mode$m.log"
        node "$HOSTS/jit_runbin.js" "$GUESTS/$t.bin" $m > "$run_log" 2>&1 \
            || bad "$t/$m process exit"
        grep -q PASS-marker "$run_log" || bad "$t/$m"
        if [ "$t" = test ]; then
            # Per-stage checksums must match the native build of the same C.
            logged "$GUESTS/host_ref" | awk '/^stage/{printf "result%s %s\n", substr($1, 6), $2}' > want.txt \
                || bad "test/$m native reference"
            grep -E '^result[0-5] ' "$run_log" | diff -q want.txt - >/dev/null || bad "test/$m checksums"
        fi
    done
done

# Strict mix-log comparison against the reference programs.
ref() {  # ref <guest> <reference program> <log it writes>
    rm -f "$3"
    logged "$GUESTS/$2" >/dev/null || { bad "$2"; return; }
    mv "$3" "$1_expect.log"
}
ref neon4 neon4_ref ref_mix.log
ref neon5 neon5_ref ref_mix.log
ref neon6 neon6_ref ref_mix.log
ref fp16 fp16_ref ref_mix.log
ref media media_ref_log ref_mix.log
ref neon neon_ref_log ref_mix.log
ref neon2 neon2_ref_log ref2_mix.log
ref neon3 neon3_ref_log ref3_mix.log
ref qc qc_ref qc_ref.log
for t in neon neon2 neon3 neon4 neon5 neon6 media fp16 qc; do
    for m in 0 1 2; do
        rm -f guest_mix.log
        node "$HOSTS/jit_runbin.js" "$GUESTS/${t}_log.bin" $m >/dev/null 2>&1 \
            || bad "mix-log $t/$m process exit"
        cmp -s "${t}_expect.log" guest_mix.log || bad "mix-log $t/$m"
        [ ! -f guest_mix.log ] || mv guest_mix.log "guests/a32-$t-mode$m.mix.log"
    done
done

# A64 guests (tests/guest64): the same C workloads, checked against the same
# native reference programs.
GUESTS64="$NATIVE/tests/guest64"
if [ -f "$GUESTS64/test.bin" ]; then
    logged "$GUESTS/host_ref" | awk '/^stage/{printf "result%s %s\n", substr($1, 6), $2}' > want_test.txt \
        || bad "a64 native reference"
    fp_expected="$(logged "$GUESTS/fp_ref")" || bad "a64 fp native reference"
    printf 'result0 %s\n' "$fp_expected" > want_fp.txt
    for t in test test_o3 fp fp_fma; do
        for m in 0 1 2; do
            run_log="guests/a64-$t-mode$m.log"
            node "$HOSTS/a64_runbin.js" "$GUESTS64/$t.bin" $m > "$run_log" 2>&1 \
                || bad "a64 $t/$m process exit"
            grep -q PASS-marker "$run_log" || { bad "a64 $t/$m"; continue; }
            case $t in
            test*) grep -E '^result[0-5] ' "$run_log" | diff -q want_test.txt - >/dev/null || bad "a64 $t/$m checksums" ;;
            fp) grep -E '^result0 ' "$run_log" | diff -q want_fp.txt - >/dev/null || bad "a64 fp/$m checksum" ;;
            fp_fma) grep -E '^result0 ' "$run_log" > "fma_$m.txt"; cmp -s fma_0.txt "fma_$m.txt" || bad "a64 fp_fma/$m differs from mode 0" ;;
            esac
        done
    done
elif [ "$strict" -eq 1 ]; then
    bad "a64 guest programs missing"
else
    skip "A64 guests (install ziglang and rebuild the native tree)"
fi

# Differential fuzzing against Unicorn (needs: pip install unicorn capstone).
if python3 -c "import unicorn, capstone" 2>/dev/null; then
    for m in 0 1 2; do
        fuzz "a64-straight-mode$m-seed7$m" "$ROOT/tests/a64/fuzz.py" \
            --host "$HOSTS/a64_fuzz.js" --count 1500 --seed 7$m --len 4 --mode $m --max-unsupported-rate 0.05
        # Self-loop register caching / deferred flags, then multi-block regions.
        fuzz "a64-loop-mode$m-seed8$m" "$ROOT/tests/a64/fuzz.py" \
            --host "$HOSTS/a64_fuzz.js" --count 1000 --seed 8$m --len 6 --loop 3 --mode $m --max-unsupported-rate 0.05
        fuzz "a64-region-mode$m-seed9$m" "$ROOT/tests/a64/fuzz.py" \
            --host "$HOSTS/a64_fuzz.js" --count 1000 --seed 9$m --len 6 --loop 3 --branches 0.4 --mode $m --max-unsupported-rate 0.05
    done
    # A32 / Thumb, straight-line and in loops with branches (regions).
    for m in 0 1 2; do
        for isa in "" --thumb; do
            arch="${isa#--}"
            [ -n "$arch" ] || arch=a32
            maximum=0.30
            [ "$arch" != thumb ] || maximum=0.10
            fuzz "$arch-straight-mode$m-seed5$m" "$ROOT/tests/a32/fuzz.py" \
                --host "$HOSTS/a32_fuzz.js" --count 1500 --seed 5$m --len 3 --mode $m $isa --max-unsupported-rate "$maximum"
            fuzz "$arch-region-mode$m-seed6$m" "$ROOT/tests/a32/fuzz.py" \
                --host "$HOSTS/a32_fuzz.js" --count 800 --seed 6$m --len 5 --loop 3 --branches 0.4 --mode $m $isa --max-unsupported-rate "$maximum"
        done
    done
    python3 "$ROOT/tests/fuzz/check_corpus.py" \
        --a32-host "$HOSTS/a32_fuzz.js" --a64-host "$HOSTS/a64_fuzz.js" \
        > corpus.out 2>&1 || { bad checked_in_fuzz_corpus; cat corpus.out; }
else
    skip "A64 / A32 fuzz (python3 -m pip install unicorn capstone)"
    [ "$strict" -eq 1 ] && bad "fuzz dependencies missing"
fi

# Browser example's JIT build on the demo guests (both architectures, all
# memory modes) under Node; the output must match the native build.
if [ -f "$EM/examples/browser/dist/arm_jit.js" ]; then
    python3 "$ROOT/examples/c-bench/reference.py" --output c-bench-reference.json || bad c_bench_reference
    cmp -s c-bench-reference.json "$EM/examples/browser/dist/c-bench-reference.json" || bad c_bench_reference_artifact
    node "$ROOT/examples/browser/fault_test.mjs" "$EM/examples/browser" || bad browser_fault_halts
    browser_runs=1
    [ "$strict" -eq 1 ] && browser_runs=3
    node "$ROOT/examples/browser/bench.mjs" "$EM/examples/browser" "$browser_runs" \
        --json browser-benchmark.json > demo.out 2>&1 || bad "browser benchmark process exit"
    node "$ROOT/tests/bench/browser_validate.mjs" browser-benchmark.json >/dev/null \
        || { bad "browser benchmark schema"; cat demo.out; }
    grep -q 'FAIL' demo.out && { bad "browser demo"; cat demo.out; }
    [ "$(grep -c 'PASS' demo.out)" -ge 4 ] || { bad "browser demo (missing results)"; cat demo.out; }
elif [ "$strict" -eq 1 ]; then
    bad "browser example artifact missing"
else
    skip "browser host (build the Emscripten example)"
fi

# External CoreMark is opt-in; an explicitly requested workload must pass.
# Keep CoreMark evidence with requested artifacts, or separately when WORK is temporary.
if [ -n "$coremark" ]; then
    if [ "$keep" -eq 1 ]; then
        coremark_result="$WORK/coremark"
        mkdir "$coremark_result"
    else
        coremark_result="$(mktemp -d)"
    fi
    echo "CoreMark evidence: $coremark_result"
    node "$ROOT/tests/bench/coremark/run.mjs" "$EM/examples/browser" "$coremark" \
        --runs "$benchmark_runs" --output "$coremark_result/report.json" || bad coremark_workload
else
    echo "CoreMark not requested (use --coremark WORKLOAD_DIR); application gate not covered"
fi

echo "regression done fail=$fail"
exit $fail
