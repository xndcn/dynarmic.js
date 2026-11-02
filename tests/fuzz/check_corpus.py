#!/usr/bin/env python3
"""Replay the checked-in semantic corpus against the Emscripten hosts.

Each case is sent through replay.py, which recomputes its Unicorn oracle. A
stale expected-state file therefore cannot make the corpus pass silently.
"""

import argparse
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
CORPUS = ROOT / "tests" / "fuzz" / "corpus"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--a32-host", required=True)
    ap.add_argument("--a64-host", required=True)
    args = ap.parse_args()
    cases = sorted(CORPUS.glob("**/case-*.json"))
    if not cases:
        raise SystemExit("checked-in fuzz corpus is empty")
    for case in cases:
        with case.open() as stream:
            payload = json.load(stream)
        if payload.get("schema") != "dynarmic.js/fuzz-case-v1":
            raise SystemExit(f"{case}: unsupported corpus schema")
        architecture = payload.get("architecture")
        host = args.a32_host if architecture == "a32" else args.a64_host if architecture == "a64" else None
        if host is None:
            raise SystemExit(f"{case}: unsupported architecture")
        command = [sys.executable, str(ROOT / "tests" / "fuzz" / "replay.py"), str(case), "--host", host]
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
        if result.stdout:
            print(result.stdout, end="")
        if result.returncode != 0:
            if result.stderr:
                print(result.stderr, end="", file=sys.stderr)
            raise SystemExit(f"corpus replay failed: {case}")
    print(f"PASS checked-in fuzz corpus cases={len(cases)}")


if __name__ == "__main__":
    main()
