#!/usr/bin/env python3
"""Independent integer oracle, including exact rational binary64 work."""
import argparse
import json
from pathlib import Path

MASK = (1 << 32) - 1
WORDS = 8192
SEED = 0x12345678
CASES = [
    {"id": "integer", "index": 0, "title": "Integer mix", "description": "Dependent 32-bit multiply, XOR and rotate."},
    {"id": "branch", "index": 1, "title": "Data-dependent branches", "description": "Seeded branch choices with two distinct non-inlined call paths."},
    {"id": "stream", "index": 2, "title": "Sequential array", "description": "Read, update and sum an 8,192-word array in order."},
    {"id": "pointer", "index": 3, "title": "Pointer chase", "description": "Follow one shuffled 8,192-entry cycle with dependent loads."},
    {"id": "float", "index": 4, "title": "Binary64 arithmetic", "description": "Exactly representable dyadic products and sums from a seeded sequence."},
]
PROFILES = {
    "quick": {"integer": 4000000, "branch": 1000000, "stream": 8000000, "pointer": 4000000, "float": 4000000},
    "full": {"integer": 16000000, "branch": 4000000, "stream": 32000000, "pointer": 16000000, "float": 16000000},
}


def next_value(value):
    return (value * 1664525 + 1013904223) & MASK


def checksum(case, count, seed):
    if case == "integer":
        value = seed
        for _ in range(count):
            value = next_value(value)
            value ^= value >> 13
            value = ((value << 7) | (value >> 25)) & MASK
        return value
    if case == "branch":
        value = state = seed
        for _ in range(count):
            state = next_value(state)
            if state & (1 << 31):
                value ^= state
                value = ((value ^ (value >> 7)) * 3 + 0x9e3779b9) & MASK
            else:
                value = (value + state) & MASK
                value = (((value << 5) | (value >> 27)) + 0x7f4a7c15) & MASK
        return value ^ state
    if case == "stream":
        data, state = [], seed
        for _ in range(WORDS):
            state = next_value(state)
            data.append(state)
        total = seed
        for i in range(count):
            index = i % WORDS
            value = (data[index] + i) & MASK
            data[index] = value ^ (value >> 11)
            total = (total + value) & MASK
        return total ^ data[(count - 1) % WORDS]
    if case == "pointer":
        order, state = list(range(WORDS)), seed
        for i in range(WORDS):
            state = next_value(state)
            j = (state >> 8) % WORDS
            order[i], order[j] = order[j], order[i]
        links = dict(zip(order, order[1:] + order[:1]))
        index, total = seed % WORDS, seed
        for _ in range(count):
            index = links[index]
            total = ((total ^ index) + 0x9e3779b9) & MASK
        return total ^ index
    if case == "float":
        squares_numerator = offsets_numerator = 0
        state = seed
        for _ in range(count):
            state = next_value(state)
            value = state >> 22
            squares_numerator += value * value  # denominator 2^20
            offsets_numerator += value + 512  # denominator 2^12
        # No floating arithmetic here: each C binary64 operation is exact for
        # the bounded count. The final unsigned casts truncate positive values.
        return (squares_numerator >> 14) ^ (offsets_numerator >> 8) ^ state
    raise ValueError(case)


def reference():
    return {"schema": "dynarmic.js/c-bench-reference-v1", "cases": CASES,
            "profiles": {profile: {"iterations": counts, "seed": SEED,
                          "expected": {case: checksum(case, count, SEED) for case, count in counts.items()}}
                         for profile, counts in PROFILES.items()},
            "protocol": {"firstRuns": 1, "warmupRuns": 2, "measuredRuns": 3, "totalRuns": 6, "rounds": 3,
                         "guestRamBytes": 16777216, "mailboxAddress": 61440, "magic": 0x43424e31,
                         "mailbox": {"request": ["magic", "caseIndex", "iterations", "seed"],
                                     "statusWord": 4, "completedRunsWord": 5, "checksumsWord": 6,
                                     "status": {"running": 1, "complete": 2, "invalid": 3}},
                         "arm": {"entry": 65536, "markSvc": 3, "exitSvc": 0},
                         "x86": {"markPort": 0xe200, "exitPort": 0xe208, "markValues": [0, 1]},
                         "wasm": {"initExport": "bench_init", "runExport": "bench_run"}},
            "oracle": "Independent Python unsigned arithmetic; binary64 case uses exact rational integer numerators"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = json.dumps(reference(), indent=2) + "\n"
    if args.output:
        args.output.write_text(result)
    else:
        print(result, end="")
