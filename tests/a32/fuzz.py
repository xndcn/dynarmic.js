#!/usr/bin/env python3
"""Differential fuzzer: A32 wasm JIT (tests/hosts/a32_fuzz.cpp) vs Unicorn.

Random ARM or Thumb instruction sequences (conditional execution and Thumb
IT blocks included; branches excluded unless asked for) run from random
register state in both; final R0-R14, CPSR.NZCVQ/GE, FPSCR.NZCV/QC, D0-D31
and the data page must agree.

    python3 tests/a32/fuzz.py --host build-em/tests/a32_fuzz.js --count 2000 --seed 1
    python3 tests/a32/fuzz.py ... --thumb               # Thumb-2
    python3 tests/a32/fuzz.py ... --loop 3 --branches 0.3  # loops / regions
    python3 tests/a32/fuzz.py ... --word e1a00000       # one instruction

As for tests/a64/fuzz.py, NaN payloads and FPSR cumulative exception bits
are tolerated by default (--strict-nan / --strict-fpscr).

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
from unicorn import arm_const as A

CODE_BASE = 0x1000
DATA_BASE = 0x100000
DATA_SIZE = 0x1000
COUNTER_ARM = 11  # loop counter register (--loop), sequences touching it are skipped
COUNTER_THUMB = 7

GPR_IDS = [getattr(A, f"UC_ARM_REG_R{i}") for i in range(13)] + [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR]
D_IDS = [getattr(A, f"UC_ARM_REG_D{i}") for i in range(32)]

CS_ARM = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
CS_THUMB = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
for cs in (CS_ARM, CS_THUMB):
    cs.detail = True


def insn_of(code, thumb):
    for insn in (CS_THUMB if thumb else CS_ARM).disasm(code, 0):
        return insn
    return None


# Generic coprocessor instructions: the fuzz host configures no coprocessors
# (they only raise), and upstream's Thumb LDC/STC with Rn = PC and writeback
# (UNPREDICTABLE) trips an assertion in the frontend. Matched by encoding
# (coprocessor space with a coprocessor other than 10/11 = VFP/NEON), since
# capstone names later extensions there (e.g. VUDOT) that upstream decodes as
# LDC/STC.
COPROC_MNEMONICS = ("ldc", "stc", "mcr", "mrc", "cdp")

# Upstream's A32 frontend emits the fused (AArch64) step ops for VRECPS /
# VRSQRTS, whereas AArch32 defines them unfused: rare 1 ulp differences.
UPSTREAM_DEVIATION_MNEMONICS = ("vrecps", "vrsqrts")


def in_generic_coproc_space(code, thumb):
    if thumb:
        if len(code) != 4:
            return False
        hw1, hw2 = struct.unpack("<HH", code)
        top = hw1 >> 8
        if (top & 0xEC) != 0xEC or (top & 0xEF) == 0xEF:  # not coproc space, or NEON data processing
            return False
        cp = (hw2 >> 8) & 0xF
    else:
        w = struct.unpack("<I", code)[0]
        if ((w >> 24) & 0xF) not in (0b1100, 0b1101, 0b1110):
            return False
        cp = (w >> 8) & 0xF
    return cp not in (10, 11)


def is_generic_coproc(code, thumb):
    if in_generic_coproc_space(code, thumb):
        return True
    insn = insn_of(code, thumb)
    return insn is not None and insn.mnemonic.startswith(COPROC_MNEMONICS + UPSTREAM_DEVIATION_MNEMONICS)


def disasm(code, thumb):
    insn = insn_of(code, thumb)
    return f"{insn.mnemonic} {insn.op_str}" if insn else "<undecodable>"


def touches(code, thumb, reg):
    insn = insn_of(code, thumb)
    if insn is None:
        return True
    try:
        read, written = insn.regs_access()
    except capstone.CsError:
        return True
    name = {7: capstone.arm.ARM_REG_R7, 11: capstone.arm.ARM_REG_R11}[reg]
    return name in set(read) | set(written)


# ---------------------------------------------------------------------------
# Instruction generation
# ---------------------------------------------------------------------------

def random_cond(rng):
    return 0xE if rng.random() < 0.8 else rng.randrange(14)


def random_arm(rng):
    """A random ARM instruction from the data-processing, load/store, media
    and VFP/NEON spaces (no branches; PC destinations mostly avoided)."""
    kind = rng.choices(["dp", "ldst", "media", "vfp", "neon", "neon_ldst"], [5, 3, 2, 3, 4, 1])[0]
    w = rng.getrandbits(32)
    if kind == "dp":
        w = (w & ~(0b11 << 26))  # bits 27:26 = 00
    elif kind == "ldst":
        if rng.random() < 0.5:
            w = (w & ~(0b111 << 25)) | (0b010 << 25)  # immediate offset
        else:
            w = ((w & ~(0b111 << 25)) | (0b011 << 25)) & ~(1 << 4)  # register offset
    elif kind == "media":
        w = (w & ~(0b111 << 25)) | (0b011 << 25) | (1 << 4)
    elif kind == "vfp":
        w = (w & ~(0b1111 << 24)) | (0b1110 << 24)
        w = (w & ~(0b111 << 9)) | (0b101 << 9)  # coprocessor 10/11
    if kind in ("neon", "neon_ldst"):
        top = 0b1111001 if kind == "neon" else 0b11110100
        width = 7 if kind == "neon" else 8
        return (w & ((1 << (32 - width)) - 1)) | (top << (32 - width))
    w = (w & 0x0FFFFFFF) | (random_cond(rng) << 28)
    # Rd / Rt = PC turns most of these into branches.
    if (w >> 12) & 0xF == 0xF and rng.random() < 0.95:
        w = (w & ~(0xF << 12)) | (rng.randrange(15) << 12)
    return w


def random_thumb(rng, allow_it):
    """A random Thumb instruction as bytes (16 or 32 bits), no branches."""
    while True:
        if rng.random() < 0.45:
            hw1 = rng.choice([0b11101, 0b11110, 0b11111]) << 11 | rng.getrandbits(11)
            hw2 = rng.getrandbits(16)
            if (hw1 >> 11) == 0b11110 and (hw2 >> 15):
                continue  # branches and miscellaneous control
            return struct.pack("<HH", hw1, hw2)
        hw = rng.getrandbits(16)
        top5 = hw >> 11
        if top5 in (0b11101, 0b11110, 0b11111):
            continue  # 32-bit prefixes
        if top5 == 0b11100 or (hw >> 12) == 0b1101:
            continue  # B, B<cond>, SVC, UDF
        if (hw >> 8) & 0xF5 == 0xB1:
            continue  # CBZ / CBNZ
        if (hw >> 7) in (0b010001110, 0b010001111):
            continue  # BX / BLX
        if (hw >> 8) == 0xBF and (hw & 0xF):
            if not allow_it:
                continue
        if (hw >> 6) & 0b1111111111 == 0b0100011111 or ((hw >> 8) == 0b01000110 and (hw & 0x87) == 0x87):
            continue  # ADD/MOV to PC
        return struct.pack("<H", hw)


def it_length(insn):
    """Instructions covered by a Thumb IT instruction, or 0."""
    if len(insn) != 2:
        return 0
    hw = struct.unpack("<H", insn)[0]
    if (hw >> 8) != 0xBF or (hw & 0xF) == 0:
        return 0
    mask = hw & 0xF
    return 4 - ((mask & -mask).bit_length() - 1)


def close_it_blocks(rng, insns, allow_it):
    """Drops ITs inside IT blocks (unpredictable) and pads an IT block left
    open at the end with NOPs, so the final SVC is never conditional."""
    out = []
    pending = 0
    for insn in insns:
        n = it_length(insn)
        if n and pending:
            insn = random_thumb(rng, allow_it=False)
            n = 0
        if pending:
            pending -= 1
        out.append(insn)
        pending = max(pending, n)
    out += [struct.pack("<H", 0xBF00)] * pending  # nop
    return out


def forward_branch(rng, thumb, skip_bytes):
    cond = rng.randrange(14)
    if thumb:
        # b<cond> label: target = pc + 4 + imm8 * 2, pc = this instruction
        imm8 = (skip_bytes + 2 - 4) // 2
        return struct.pack("<H", 0xD000 | (cond << 8) | (imm8 & 0xFF))
    # b<cond> label: target = pc + 8 + imm24 * 4
    imm24 = (skip_bytes + 4 - 8) // 4
    return struct.pack("<I", (cond << 28) | 0x0A000000 | (imm24 & 0xFFFFFF))


def generate(rng, args):
    thumb = args.thumb
    if args.word:
        insns = []
        for w in args.word.split(","):
            v = int(w, 16)
            insns.append(struct.pack("<H", v) if thumb and len(w) <= 4 else (struct.pack("<HH", v >> 16, v & 0xFFFF) if thumb else struct.pack("<I", v)))
    else:
        n = rng.randint(1, args.len)
        insns = [random_thumb(rng, allow_it=not args.branches) if thumb else struct.pack("<I", random_arm(rng)) for _ in range(n)]
        if any(is_generic_coproc(i, thumb) for i in insns):
            return None
        if thumb:
            insns = close_it_blocks(rng, insns, allow_it=not args.branches)
    counter = COUNTER_THUMB if thumb else COUNTER_ARM
    if args.loop and any(touches(i, thumb, counter) for i in insns):
        return None
    if args.branches:
        out = []
        for i, insn in enumerate(insns):
            if rng.random() < args.branches:
                skip = sum(len(x) for x in insns[i:i + rng.randint(1, 3)])
                out.append(forward_branch(rng, thumb, skip))
            out.append(insn)
        insns = out
    if args.loop:
        body = b"".join(insns)
        if thumb:
            head = struct.pack("<H", 0x2000 | (counter << 8) | args.loop)      # movs r7, #N
            tail = struct.pack("<H", 0x3800 | (counter << 8) | 1)               # subs r7, #1
            back = -(len(body) + 2 + 4)                                         # to the body start
            tail += struct.pack("<H", 0xD100 | ((back // 2) & 0xFF))           # bne
        else:
            head = struct.pack("<I", 0xE3A00000 | (counter << 12) | args.loop)  # mov r11, #N
            tail = struct.pack("<I", 0xE2500001 | (counter << 16) | (counter << 12))  # subs r11, r11, #1
            back = -(len(body) + 4 + 8)
            tail += struct.pack("<I", 0x1A000000 | ((back // 4) & 0xFFFFFF))  # bne
        return head + body + tail, insns
    return b"".join(insns), insns


# ---------------------------------------------------------------------------
# State
# ---------------------------------------------------------------------------

def interesting_u32(rng):
    r = rng.random()
    if r < 0.3:
        return rng.choice([0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000,
                           0xFFFFFFFF, 0x3F800000, 0xBF800000, 0x7F800000, 0x7FC00000, 31, 32, 33])
    return rng.getrandbits(rng.choice([8, 16, 32, 32]))


def random_lane64(rng):
    r = rng.random()
    if r < 0.3:
        return struct.unpack("<Q", struct.pack("<d", rng.uniform(-1000, 1000)))[0]
    if r < 0.6:
        lo, hi = (struct.unpack("<I", struct.pack("<f", rng.uniform(-100, 100)))[0] for _ in range(2))
        return lo | (hi << 32)
    return interesting_u32(rng) | (interesting_u32(rng) << 32)


def random_state(rng):
    r = []
    for i in range(15):
        if rng.random() < 0.4 or i == 13:
            r.append((DATA_BASE + 0x400 + rng.randrange(0, 0x400)) & ~rng.choice([0, 3, 7]))
        else:
            r.append(interesting_u32(rng))
    flags = (rng.getrandbits(5) << 27) | (rng.getrandbits(4) << 16)  # NZCVQ, GE
    d = [random_lane64(rng) for _ in range(32)]
    mem = bytes(rng.getrandbits(8) for _ in range(DATA_SIZE))
    return {"r": r, "flags": flags, "d": d, "mem": mem}


def run_unicorn(code, st, thumb):
    uc = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_THUMB if thumb else unicorn.UC_MODE_ARM)
    uc.ctl_set_cpu_model(A.UC_CPU_ARM_MAX)
    uc.mem_map(CODE_BASE, 0x1000)
    uc.mem_map(DATA_BASE, DATA_SIZE)
    # The host places an svc right after the code; literal loads may read it.
    uc.mem_write(CODE_BASE, code + (struct.pack("<H", 0xDF00) if thumb else struct.pack("<I", 0xEF000000)))
    uc.mem_write(DATA_BASE, st["mem"])
    uc.reg_write(A.UC_ARM_REG_C1_C0_2, uc.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xF << 20))
    uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
    cpsr = uc.reg_read(A.UC_ARM_REG_CPSR)
    uc.reg_write(A.UC_ARM_REG_CPSR, (cpsr & ~0xF80F0000) | st["flags"])
    for rid, val in zip(GPR_IDS, st["r"]):
        uc.reg_write(rid, val)
    uc.reg_write(A.UC_ARM_REG_FPSCR, 0)
    for rid, val in zip(D_IDS, st["d"]):
        uc.reg_write(rid, val)
    end = CODE_BASE + len(code)
    try:
        uc.emu_start(CODE_BASE | (1 if thumb else 0), end, count=4000)
    except unicorn.UcError:
        return None
    if uc.reg_read(A.UC_ARM_REG_PC) != end:
        return None
    return {
        "r": [uc.reg_read(rid) for rid in GPR_IDS],
        "cpsr": uc.reg_read(A.UC_ARM_REG_CPSR),
        "fpscr": uc.reg_read(A.UC_ARM_REG_FPSCR),
        "d": [uc.reg_read(rid) for rid in D_IDS],
        "mem": bytes(uc.mem_read(DATA_BASE, DATA_SIZE)),
    }


def pack_case(code, st, thumb):
    padded = code + b"\0" * (-len(code) % 4)
    out = struct.pack("<II", 1 if thumb else 0, len(code)) + padded
    cpsr = st["flags"] | 0x1F | (0x20 if thumb else 0)
    out += struct.pack("<15I", *st["r"]) + struct.pack("<3I", cpsr, 0, 0)
    ext = []
    for d in st["d"]:
        ext += [d & 0xFFFFFFFF, d >> 32]
    out += struct.pack("<64I", *ext) + st["mem"]
    return out


RESULT_FMT = "<I16III64I"
RESULT_SIZE = struct.calcsize(RESULT_FMT) + DATA_SIZE


def unpack_result(blob):
    f = struct.unpack_from(RESULT_FMT, blob)
    ext = f[19:83]
    return f[0], {
        "r": list(f[1:16]),
        "pc": f[16],
        "cpsr": f[17],
        "fpscr": f[18],
        "d": [ext[2 * i] | (ext[2 * i + 1] << 32) for i in range(32)],
        "mem": blob[struct.calcsize(RESULT_FMT):],
    }


def is_nan32(b):
    return (b & 0x7F800000) == 0x7F800000 and (b & 0x007FFFFF) != 0


def is_nan64(b):
    return (b & 0x7FF0000000000000) == 0x7FF0000000000000 and (b & 0x000FFFFFFFFFFFFF) != 0


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


def diff_states(want, got, args):
    diffs = []
    for i in range(15):
        if want["r"][i] != got["r"][i]:
            diffs.append(f"r{i}: want {want['r'][i]:08x} got {got['r'][i]:08x}")
    if (want["cpsr"] ^ got["cpsr"]) & 0xF80F0000:
        diffs.append(f"cpsr: want {want['cpsr'] & 0xF80F0000:08x} got {got['cpsr'] & 0xF80F0000:08x}")
    mask = 0xFFFFFFFF if args.strict_fpscr else 0xF8000000
    if (want["fpscr"] ^ got["fpscr"]) & mask:
        diffs.append(f"fpscr: want {want['fpscr']:08x} got {got['fpscr']:08x}")
    for i in range(32):
        if not lanes_match(want["d"][i], got["d"][i], args.strict_nan):
            diffs.append(f"d{i}: want {want['d'][i]:016x} got {got['d'][i]:016x}")
    if want["mem"] != got["mem"]:
        first = next(i for i in range(DATA_SIZE) if want["mem"][i] != got["mem"][i])
        diffs.append(f"mem: first difference at {DATA_BASE + first:#x}: want {want['mem'][first]:02x} got {got['mem'][first]:02x}")
    return diffs


def show_code(code, thumb):
    lines = []
    for insn in (CS_THUMB if thumb else CS_ARM).disasm(code, CODE_BASE):
        lines.append(f"    {insn.address:05x}: {insn.bytes.hex():<10} {insn.mnemonic} {insn.op_str}")
    return "\n".join(lines) if lines else f"    {code.hex()} <undecodable>"


def save_corpus_case(directory, index, code, st, args, diffs):
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, f"case-{index:04d}.json")
    payload = {
        "schema": "dynarmic.js/fuzz-case-v1", "architecture": "a32",
        "thumb": args.thumb, "mode": str(args.mode),
        "strictNan": args.strict_nan, "strictFpscr": args.strict_fpscr,
        "code": code.hex(), "state": {**st, "mem": st["mem"].hex()},
        "diffs": diffs,
    }
    with open(path, "w") as f:
        json.dump(payload, f, indent=2, sort_keys=True)
        f.write("\n")
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="build-em/tests/a32_fuzz.js")
    ap.add_argument("--count", type=positive_int, default=1000, help="oracle-accepted cases to generate")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--len", type=positive_int, default=4)
    ap.add_argument("--thumb", action="store_true")
    ap.add_argument("--word", help="comma-separated instructions (hex) to test")
    ap.add_argument("--mode", default="0")
    ap.add_argument("--loop", type=int, default=0)
    ap.add_argument("--branches", type=float, default=0)
    ap.add_argument("--strict-nan", action="store_true")
    ap.add_argument("--strict-fpscr", action="store_true")
    ap.add_argument("--show", type=int, default=20)
    ap.add_argument("--show-unsupported", type=int, default=0, metavar="N",
                    help="list the first N cases the JIT reported as unsupported")
    ap.add_argument("--save-corpus", metavar="DIR", help="save mismatches as replayable JSON cases")
    ap.add_argument("--max-unsupported-rate", type=unsupported_rate, default=1.0, metavar="FRACTION",
                    help="maximum fraction of host cases not compared, from 0 to 1 (default: 1)")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    cases = []
    attempts = 0
    while len(cases) < args.count and attempts < args.count * 200:
        attempts += 1
        gen = generate(rng, args)
        if gen is None:
            continue
        code, _ = gen
        st = random_state(rng)
        want = run_unicorn(code, st, args.thumb)
        if want is not None:
            cases.append((code, st, want))
        if args.word and attempts >= args.count:
            break

    generation_error = generation_failure(args.count, len(cases), bool(args.word))
    if generation_error:
        print(f"cases={len(cases)} (of {attempts} generated) compared=0 unsupported=0 mismatches=0")
        print(f"FAIL coverage: {generation_error}")
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        cases_path, results_path, log_path = (os.path.join(tmp, n) for n in ("cases.bin", "results.bin", "host.log"))
        with open(cases_path, "wb") as f:
            for code, st, _ in cases:
                f.write(pack_case(code, st, args.thumb))
        with open(log_path, "w") as log:
            rc = subprocess.run(["node", args.host, cases_path, results_path, args.mode], stdout=log, stderr=subprocess.STDOUT).returncode
        with open(results_path, "rb") as f:
            results = f.read()
        if rc != 0 or len(results) != RESULT_SIZE * len(cases):
            ran = len(results) // RESULT_SIZE
            print(f"host failed (rc={rc}) after {ran} of {len(cases)} cases; first unfinished case:")
            if ran < len(cases):
                print(show_code(cases[ran][0], args.thumb))
            with open(log_path) as log:
                text = log.read()
            print("\n".join(l[:300] for l in text.split("\n") if "Error" in l or "Message" in l or "assert" in l)[:3000])
            return 1

    unsupported = 0
    mismatches = []
    for i, (code, st, want) in enumerate(cases):
        status, got = unpack_result(results[i * RESULT_SIZE:(i + 1) * RESULT_SIZE])
        if status != 0:
            if unsupported < args.show_unsupported:
                print(f"--- unsupported (stopped at pc {got['pc']:#x})")
                print(show_code(code, args.thumb))
            unsupported += 1
            continue
        diffs = diff_states(want, got, args)
        if diffs:
            mismatches.append((code, st, diffs))

    compared = len(cases) - unsupported
    print(f"cases={len(cases)} (of {attempts} generated) compared={compared} "
          f"unsupported={unsupported} mismatches={len(mismatches)}")
    coverage_error = coverage_failure(len(cases), unsupported, args.max_unsupported_rate)
    if coverage_error:
        print(f"FAIL coverage: {coverage_error}")
    for i, (code, st, diffs) in enumerate(mismatches[:args.show]):
        print("---")
        print(show_code(code, args.thumb))
        for d in diffs[:8]:
            print(f"    {d}")
    if args.save_corpus:
        for i, (code, st, diffs) in enumerate(mismatches):
            print(f"saved {save_corpus_case(args.save_corpus, i, code, st, args, diffs)}")
    return 1 if mismatches or coverage_error else 0


if __name__ == "__main__":
    sys.exit(main())
