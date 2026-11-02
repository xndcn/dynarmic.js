#!/usr/bin/env python3
"""Differential fuzzer: A64 wasm JIT (tests/hosts/a64_fuzz.cpp) vs Unicorn.

Random straight-line instruction sequences (no branches / system
instructions) run from random register state in both; final GPRs, SP, NZCV,
vector registers, FPSR.QC and the data page must agree.

    python3 tests/a64/fuzz.py --host build-em/tests/a64_fuzz.js --count 2000 --seed 1
    python3 tests/a64/fuzz.py ... --class simd --len 1          # one encoding class
    python3 tests/a64/fuzz.py ... --word 4e208400               # one instruction
    python3 tests/a64/fuzz.py ... --mode 1                      # page-table memory
    python3 tests/a64/fuzz.py ... --loop 3                      # run each sequence in a loop

--loop N wraps the sequence in `mov x28, #N; 1: <seq>; subs x28, x28, #1;
b.ne 1b`, which the JIT compiles as a self-loop (registers cached in wasm
locals, deferred flags); sequences touching x28 are skipped.
--branches P inserts, before each instruction with probability P, a forward
b.cond / cbz / cbnz / tbz / tbnz skipping 1..3 instructions: the code splits
into several blocks, which the JIT compiles as one region.

Known, intentionally tolerated differences:
  * NaN payloads (the JIT uses wasm float ops, which do not follow ARM's NaN
    propagation rules): vector lanes that are NaN on both sides compare equal,
    at f32 and f64 granularity, as do NaN scalars in the low lanes.
  * FPSR cumulative exception bits (IOC, DZC, ...), which wasm float ops do
    not produce; only QC (bit 27) is compared.
Pass --strict-nan / --strict-fpsr (or --strict-fp for both) to compare those
too.

Requires: pip install unicorn capstone
"""

import argparse
import json
import os
import random
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "fuzz"))
from campaign import coverage_failure, generation_failure, positive_int, unsupported_rate

import capstone
import unicorn
from unicorn import arm64_const as A

CODE_BASE = 0x1000
DATA_BASE = 0x100000
DATA_SIZE = 0x1000
SVC0 = 0xD4000001

# Encoding classes by op0 (bits 28:25), with a weight each.
CLASSES = {
    "dpimm": (0b1000, 0b1110, 3),   # 100x data processing - immediate
    "dpreg": (0b0101, 0b0111, 4),   # x101 data processing - register
    "ldst": (0b0100, 0b0101, 3),    # x1x0 loads and stores
    "simd": (0b0111, 0b0111, 6),    # x111 SIMD & FP
}

GPR_IDS = [getattr(A, f"UC_ARM64_REG_X{i}") for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30]
Q_IDS = [getattr(A, f"UC_ARM64_REG_Q{i}") for i in range(32)]

CS = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
CS.detail = True
X28_REGS = {capstone.arm64.ARM64_REG_X28, capstone.arm64.ARM64_REG_W28}


def touches_x28(word):
    for insn in CS.disasm(struct.pack("<I", word), 0):
        try:
            read, written = insn.regs_access()
        except capstone.CsError:
            return True
        return bool(X28_REGS & (set(read) | set(written)))
    return True


COND_BRANCH_OPS = ("bcond", "cbz", "cbnz", "tbz", "tbnz")


def forward_branch(rng, skip):
    """A conditional branch to the instruction `skip` + 1 words ahead."""
    imm = skip + 1
    kind = rng.choice(COND_BRANCH_OPS)
    rt = rng.choice([r for r in range(31) if r != 28])
    if kind == "bcond":
        return 0x54000000 | ((imm & 0x7FFFF) << 5) | rng.randrange(14)
    if kind in ("cbz", "cbnz"):
        sf = rng.getrandbits(1)
        return (sf << 31) | 0x34000000 | ((kind == "cbnz") << 24) | ((imm & 0x7FFFF) << 5) | rt
    bit = rng.randrange(64)
    return ((bit >> 5) << 31) | 0x36000000 | ((kind == "tbnz") << 24) | ((bit & 31) << 19) | ((imm & 0x3FFF) << 5) | rt


def add_branches(rng, words, p):
    out = []
    for i, w in enumerate(words):
        remaining = len(words) - i
        if rng.random() < p:
            out.append(forward_branch(rng, rng.randint(1, min(3, remaining))))
        out.append(w)
    return out


def loop_wrap(words, count):
    """mov x28, #count; body; subs x28, x28, #1; b.ne body"""
    mov = 0xD2800000 | (count << 5) | 28
    subs = 0xF1000400 | (28 << 5) | 28
    back = -(len(words) + 1)
    bne = 0x54000001 | ((back & 0x7FFFF) << 5)
    return [mov] + words + [subs, bne]


def disasm(word):
    for insn in CS.disasm(struct.pack("<I", word), 0):
        return f"{insn.mnemonic} {insn.op_str}"
    return "<undecodable>"


def random_word(rng, cls):
    value, mask, _ = CLASSES[cls]
    word = rng.getrandbits(32)
    # op0 = bits 28:25; keep the bits selected by mask from `value`.
    op0 = ((word >> 25) & 0xF & ~mask) | (value & mask)
    word = (word & ~(0xF << 25)) | (op0 << 25)
    return word


def interesting_u64(rng):
    choice = rng.random()
    if choice < 0.25:
        return rng.choice([0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000,
                           0xFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF,
                           0x3F800000, 0xBF800000, 0x7F800000, 0x7FC00000, 0x3FF0000000000000,
                           0x7FF0000000000000, 0x7FF8000000000000, 0x4000000000000000])
    if choice < 0.5:
        return rng.getrandbits(rng.choice([8, 16, 32]))
    return rng.getrandbits(64)


def random_fp_lane64(rng):
    """Vector lane material: mostly ordinary doubles / pairs of floats."""
    r = rng.random()
    if r < 0.3:
        return struct.unpack("<Q", struct.pack("<d", rng.uniform(-1000, 1000)))[0]
    if r < 0.6:
        lo, hi = (struct.unpack("<I", struct.pack("<f", rng.uniform(-100, 100)))[0] for _ in range(2))
        return lo | (hi << 32)
    return interesting_u64(rng)


def random_state(rng):
    x = []
    for _ in range(31):
        if rng.random() < 0.4:
            # Pointer into the data page (so loads / stores mostly hit it).
            x.append(DATA_BASE + rng.randrange(0, DATA_SIZE // 2) & ~rng.choice([0, 3, 7, 15]))
        else:
            x.append(interesting_u64(rng))
    sp = DATA_BASE + 0x800
    nzcv = rng.getrandbits(4) << 28
    v = [random_fp_lane64(rng) for _ in range(64)]
    mem = bytes(rng.getrandbits(8) for _ in range(DATA_SIZE))
    return {"x": x, "sp": sp, "nzcv": nzcv, "fpcr": 0, "fpsr": 0, "v": v, "mem": mem}


def run_unicorn(words, st):
    """Returns the final state, or None if Unicorn faulted / rejected the code."""
    uc = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
    uc.mem_map(CODE_BASE, 0x1000)
    uc.mem_map(DATA_BASE, DATA_SIZE)
    code = b"".join(struct.pack("<I", w) for w in words) + struct.pack("<I", SVC0)
    uc.mem_write(CODE_BASE, code)
    uc.mem_write(DATA_BASE, st["mem"])
    uc.reg_write(A.UC_ARM64_REG_CPACR_EL1, 3 << 20)
    for rid, val in zip(GPR_IDS, st["x"]):
        uc.reg_write(rid, val)
    uc.reg_write(A.UC_ARM64_REG_SP, st["sp"])
    uc.reg_write(A.UC_ARM64_REG_NZCV, st["nzcv"])
    uc.reg_write(A.UC_ARM64_REG_FPCR, st["fpcr"])
    uc.reg_write(A.UC_ARM64_REG_FPSR, st["fpsr"])
    for i, rid in enumerate(Q_IDS):
        uc.reg_write(rid, st["v"][2 * i] | (st["v"][2 * i + 1] << 64))
    end = CODE_BASE + 4 * len(words)
    try:
        # Bounded either way: the only branch is the --loop back-edge.
        uc.emu_start(CODE_BASE, end, count=100 * len(words))
    except unicorn.UcError:
        return None
    if uc.reg_read(A.UC_ARM64_REG_PC) != end:
        return None
    q = [uc.reg_read(rid) for rid in Q_IDS]
    v = []
    for val in q:
        v += [val & (2**64 - 1), val >> 64]
    return {
        "x": [uc.reg_read(rid) for rid in GPR_IDS],
        "sp": uc.reg_read(A.UC_ARM64_REG_SP),
        "nzcv": uc.reg_read(A.UC_ARM64_REG_NZCV) & 0xF0000000,
        "fpsr": uc.reg_read(A.UC_ARM64_REG_FPSR),
        "v": v,
        "mem": bytes(uc.mem_read(DATA_BASE, DATA_SIZE)),
    }


def pack_case(words, st):
    out = struct.pack("<I", len(words)) + b"".join(struct.pack("<I", w) for w in words)
    out += struct.pack("<31Q", *st["x"]) + struct.pack("<Q", st["sp"])
    out += struct.pack("<4I", st["nzcv"], st["fpcr"], st["fpsr"], 0)
    out += struct.pack("<64Q", *st["v"]) + st["mem"]
    return out


RESULT_FMT = "<II31QQII64Q"
RESULT_SIZE = struct.calcsize(RESULT_FMT) + DATA_SIZE


def unpack_result(blob):
    fields = struct.unpack_from(RESULT_FMT, blob)
    status = fields[0]
    x = list(fields[2:33])
    sp = fields[33]
    nzcv, fpsr = fields[34], fields[35]
    v = list(fields[36:100])
    mem = blob[struct.calcsize(RESULT_FMT):]
    return status, {"x": x, "sp": sp, "nzcv": nzcv & 0xF0000000, "fpsr": fpsr, "v": v, "mem": mem}


def is_nan32(bits):
    return (bits & 0x7F800000) == 0x7F800000 and (bits & 0x007FFFFF) != 0


def is_nan64(bits):
    return (bits & 0x7FF0000000000000) == 0x7FF0000000000000 and (bits & 0x000FFFFFFFFFFFFF) != 0


def lanes_match(a, b, strict):
    if a == b:
        return True
    if strict:
        return False
    if is_nan64(a) and is_nan64(b):
        return True
    for shift in (0, 32):
        la, lb = (a >> shift) & 0xFFFFFFFF, (b >> shift) & 0xFFFFFFFF
        if la != lb and not (is_nan32(la) and is_nan32(lb)):
            return False
    return True


def diff_states(want, got, strict_nan, strict_fpsr):
    diffs = []
    for i in range(31):
        if want["x"][i] != got["x"][i]:
            diffs.append(f"x{i}: want {want['x'][i]:016x} got {got['x'][i]:016x}")
    if want["sp"] != got["sp"]:
        diffs.append(f"sp: want {want['sp']:016x} got {got['sp']:016x}")
    if want["nzcv"] != got["nzcv"]:
        diffs.append(f"nzcv: want {want['nzcv'] >> 28:04b} got {got['nzcv'] >> 28:04b}")
    fpsr_mask = 0xFFFFFFFF if strict_fpsr else 0x08000000
    if (want["fpsr"] ^ got["fpsr"]) & fpsr_mask:
        diffs.append(f"fpsr: want {want['fpsr']:08x} got {got['fpsr']:08x}")
    for i in range(32):
        w = (want["v"][2 * i], want["v"][2 * i + 1])
        g = (got["v"][2 * i], got["v"][2 * i + 1])
        if not (lanes_match(w[0], g[0], strict_nan) and lanes_match(w[1], g[1], strict_nan)):
            diffs.append(f"v{i}: want {w[1]:016x}{w[0]:016x} got {g[1]:016x}{g[0]:016x}")
    if want["mem"] != got["mem"]:
        first = next(i for i in range(DATA_SIZE) if want["mem"][i] != got["mem"][i])
        diffs.append(f"mem: first difference at {DATA_BASE + first:#x}: want {want['mem'][first]:02x} got {got['mem'][first]:02x}")
    return diffs


def generate(rng, args):
    classes = [args.cls] if args.cls else list(CLASSES)
    weights = [CLASSES[c][2] for c in classes]
    if args.word:
        words = [int(w, 16) for w in args.word.split(",")]
    else:
        n = rng.randint(1, args.len)
        words = [random_word(rng, rng.choices(classes, weights)[0]) for _ in range(n)]
    if args.loop:
        if any(touches_x28(w) for w in words):
            return None
    if args.branches:
        words = add_branches(rng, words, args.branches)
    if args.loop:
        words = loop_wrap(words, args.loop)
    return words


def save_corpus_case(directory, index, words, st, args, diffs):
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, f"case-{index:04d}.json")
    payload = {
        "schema": "dynarmic.js/fuzz-case-v1", "architecture": "a64",
        "mode": str(args.mode), "strictNan": args.strict_nan,
        "strictFpsr": args.strict_fpsr,
        "words": [f"{word:08x}" for word in words],
        "state": {**st, "mem": st["mem"].hex()}, "diffs": diffs,
    }
    with open(path, "w") as f:
        json.dump(payload, f, indent=2, sort_keys=True)
        f.write("\n")
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="build-em/tests/a64_fuzz.js")
    ap.add_argument("--count", type=positive_int, default=1000, help="oracle-accepted cases to generate")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--len", type=positive_int, default=4, help="max instructions per case")
    ap.add_argument("--class", dest="cls", choices=sorted(CLASSES))
    ap.add_argument("--word", help="comma-separated instruction words (hex) to test")
    ap.add_argument("--mode", default="0", help="host memory mode (0/1/2)")
    ap.add_argument("--loop", type=int, default=0, help="run each sequence N times in a loop")
    ap.add_argument("--branches", type=float, default=0, help="probability of a forward conditional branch before each instruction")
    ap.add_argument("--strict-fp", action="store_true", help="--strict-nan and --strict-fpsr")
    ap.add_argument("--strict-nan", action="store_true", help="compare NaN payloads")
    ap.add_argument("--strict-fpsr", action="store_true", help="compare FPSR exception bits")
    ap.add_argument("--show", type=int, default=20, help="mismatches to print")
    ap.add_argument("--save-corpus", metavar="DIR", help="save mismatches as replayable JSON cases")
    ap.add_argument("--max-unsupported-rate", type=unsupported_rate, default=1.0, metavar="FRACTION",
                    help="maximum fraction of host cases not compared, from 0 to 1 (default: 1)")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    cases = []
    attempts = 0
    while len(cases) < args.count and attempts < args.count * 200:
        attempts += 1
        words = generate(rng, args)
        if words is None:
            continue
        st = random_state(rng)
        want = run_unicorn(words, st)
        if want is not None:
            cases.append((words, st, want))
        if args.word and attempts >= args.count:
            break

    generation_error = generation_failure(args.count, len(cases), bool(args.word))
    if generation_error:
        print(f"cases={len(cases)} (of {attempts} generated) compared=0 unsupported=0 mismatches=0")
        print(f"FAIL coverage: {generation_error}")
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        cases_path = os.path.join(tmp, "cases.bin")
        results_path = os.path.join(tmp, "results.bin")
        with open(cases_path, "wb") as f:
            for words, st, _ in cases:
                f.write(pack_case(words, st))
        log_path = os.path.join(tmp, "host.log")
        with open(log_path, "w") as log:
            rc = subprocess.run(["node", args.host, cases_path, results_path, args.mode], stdout=log, stderr=subprocess.STDOUT).returncode
        with open(results_path, "rb") as f:
            results = f.read()
        if rc != 0 or len(results) != RESULT_SIZE * len(cases):
            ran = len(results) // RESULT_SIZE
            print(f"host failed (rc={rc}) after {ran} of {len(cases)} cases; first unfinished case:")
            if ran < len(cases):
                for w in cases[ran][0]:
                    print(f"    {w:08x}  {disasm(w)}")
            with open(log_path) as log:
                text = log.read()
            print("\n".join(line[:300] for line in text.split("\n") if "Error" in line or "Message" in line or "assert" in line)[:3000])
            return 1

    unsupported = 0
    mismatches = []
    for i, (words, st, want) in enumerate(cases):
        status, got = unpack_result(results[i * RESULT_SIZE:(i + 1) * RESULT_SIZE])
        if status != 0:
            unsupported += 1
            continue
        diffs = diff_states(want, got, args.strict_fp or args.strict_nan, args.strict_fp or args.strict_fpsr)
        if diffs:
            mismatches.append((words, st, diffs))

    compared = len(cases) - unsupported
    print(f"cases={len(cases)} (of {attempts} generated) compared={compared} "
          f"unsupported={unsupported} mismatches={len(mismatches)}")
    coverage_error = coverage_failure(len(cases), unsupported, args.max_unsupported_rate)
    if coverage_error:
        print(f"FAIL coverage: {coverage_error}")
    for words, st, diffs in mismatches[: args.show]:
        print("---")
        for w in words:
            print(f"    {w:08x}  {disasm(w)}")
        for d in diffs[:8]:
            print(f"    {d}")
    if args.save_corpus:
        for i, (words, st, diffs) in enumerate(mismatches):
            print(f"saved {save_corpus_case(args.save_corpus, i, words, st, args, diffs)}")
    return 1 if mismatches or coverage_error else 0


if __name__ == "__main__":
    sys.exit(main())
