#!/usr/bin/env python3
"""Replay one JSON case saved by tests/a32/fuzz.py or tests/a64/fuzz.py.

    python3 tests/fuzz/replay.py corpus/case-0000.json --host build-em/tests/a64_fuzz.js

The JSON stores guest bytes/words and the complete initial register, vector and
memory state, so replay does not depend on the original random seed or
generator filters.
"""

import argparse
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
from types import SimpleNamespace


ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load_fuzzer(architecture):
    path = os.path.join(ROOT, "tests", architecture, "fuzz.py")
    spec = importlib.util.spec_from_file_location(f"dynarmic_{architecture}_fuzz", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("case")
    ap.add_argument("--host", help="fuzz host; defaults to the architecture's build-em host")
    ap.add_argument("--mode", help="override the case's memory mode")
    args = ap.parse_args()
    with open(args.case) as f:
        case = json.load(f)
    if case.get("schema") != "dynarmic.js/fuzz-case-v1":
        raise SystemExit("unsupported corpus schema")
    arch = case.get("architecture")
    if arch not in ("a32", "a64"):
        raise SystemExit("architecture must be a32 or a64")
    fuzz = load_fuzzer(arch)
    st = dict(case["state"])
    st["mem"] = bytes.fromhex(st["mem"])
    mode = str(args.mode if args.mode is not None else case.get("mode", "0"))
    strict_nan = bool(case.get("strictNan", False))
    strict_fp = bool(case.get("strictFpsr", False))
    if arch == "a32":
        code = bytes.fromhex(case["code"])
        thumb = bool(case.get("thumb", False))
        want = fuzz.run_unicorn(code, st, thumb)
        if want is None:
            raise SystemExit("Unicorn rejected the saved A32 case")
        packed = fuzz.pack_case(code, st, thumb)
        host = args.host or os.path.join(ROOT, "build-em", "tests", "a32_fuzz.js")
        describe = fuzz.show_code(code, thumb)
    else:
        words = [int(word, 16) for word in case["words"]]
        want = fuzz.run_unicorn(words, st)
        if want is None:
            raise SystemExit("Unicorn rejected the saved A64 case")
        packed = fuzz.pack_case(words, st)
        host = args.host or os.path.join(ROOT, "build-em", "tests", "a64_fuzz.js")
        describe = "\n".join(f"    {word:08x}  {fuzz.disasm(word)}" for word in words)

    with tempfile.TemporaryDirectory() as tmp:
        cases_path, results_path = os.path.join(tmp, "cases.bin"), os.path.join(tmp, "results.bin")
        with open(cases_path, "wb") as f:
            f.write(packed)
        result = subprocess.run(["node", host, cases_path, results_path, mode], capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            return result.returncode
        with open(results_path, "rb") as f:
            blob = f.read()
    if len(blob) != fuzz.RESULT_SIZE:
        raise SystemExit(f"host returned {len(blob)} bytes, expected {fuzz.RESULT_SIZE}")
    status, got = fuzz.unpack_result(blob)
    print(describe)
    if status != 0:
        print(f"unsupported status={status}")
        return 1
    if arch == "a32":
        options = SimpleNamespace(strict_nan=strict_nan, strict_fpscr=strict_fp)
        diffs = fuzz.diff_states(want, got, options)
    else:
        diffs = fuzz.diff_states(want, got, strict_nan, strict_fp)
    if diffs:
        print("REPLAY MISMATCH")
        print("\n".join(f"  {diff}" for diff in diffs))
        return 1
    print("PASS replay")
    return 0


if __name__ == "__main__":
    sys.exit(main())
