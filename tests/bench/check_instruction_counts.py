#!/usr/bin/env python3
"""Audit benchmark denominators by executing the actual embedded guests in Unicorn.

This is a correctness check, not a performance measurement. It needs Unicorn
(the strict regression toolchain already installs it). Each memory-mode variant
uses the same instruction stream, so six executions cover all twelve cases.
"""
import argparse
import ast
from dataclasses import dataclass
from pathlib import Path
import re
import struct


@dataclass(frozen=True)
class Workload:
    host: str
    code: str
    count: str
    architecture: str
    expected_result: int


WORKLOADS = (
    Workload("jit_bench", "kCode", "kGuestInstructions", "a32", 32640),
    Workload("jit_thumb_bench", "kCode", "kGuestInstructions", "thumb", 409600),
    Workload("jit_callbench", "kCode", "kGuestInstructions", "a32", 1048576),
    Workload("jit_a64_bench", "kCode", "kGuestInstructions", "a64", 65280000),
    Workload("jit_a64_simd_bench", "kScalar", "kGuestInstructionsScalar", "a64", 4194305),
    Workload("jit_a64_simd_bench", "kNeon", "kGuestInstructionsNeon", "a64", 16777220),
)


def strip_comments(source):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", source, flags=re.S)


def constant(source, name, visiting=()):
    """Evaluate only the small integer-expression subset used by these hosts."""
    if name in visiting:
        raise ValueError(f"recursive constant: {name}")
    match = re.search(r"constexpr\s+\w+\s+" + re.escape(name) + r"\s*=\s*([^;]+);", source)
    if not match:
        raise ValueError(f"missing constant: {name}")
    expression = re.sub(r"(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", match[1]).strip()

    def evaluate(node):
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value
        if isinstance(node, ast.Name):
            return constant(source, node.id, (*visiting, name))
        if isinstance(node, ast.BinOp):
            left, right = evaluate(node.left), evaluate(node.right)
            if isinstance(node.op, ast.Add):
                return left + right
            if isinstance(node.op, ast.Sub):
                return left - right
            if isinstance(node.op, ast.Mult):
                return left * right
            if isinstance(node.op, ast.LShift) and 0 <= right <= 63:
                return left << right
        raise ValueError(f"unsupported integer expression for {name}")

    return evaluate(ast.parse(expression, mode="eval").body)


def load_workload(source, spec):
    source = strip_comments(source)
    match = re.search(r"constexpr\s+std::array<(u16|u32),\s*(\d+)>\s+" + re.escape(spec.code)
                      + r"\s*=\s*\{([^}]+)\};", source)
    if not match:
        raise ValueError(f"missing instruction array: {spec.code}")
    words = [int(word.strip(), 0) for word in match[3].split(",") if word.strip()]
    if len(words) != int(match[2]):
        raise ValueError("instruction array length mismatch")
    code = struct.pack("<" + ("H" if match[1] == "u16" else "I") * len(words), *words)
    return code, constant(source, spec.count)


def verify_workload(code, declared_count, spec):
    from unicorn import Uc, UC_ARCH_ARM, UC_ARCH_ARM64, UC_MODE_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_INTR
    from unicorn.arm_const import UC_ARM_REG_R0
    from unicorn.arm64_const import UC_ARM64_REG_X0

    a64, thumb = spec.architecture == "a64", spec.architecture == "thumb"
    engine = Uc(UC_ARCH_ARM64 if a64 else UC_ARCH_ARM, UC_MODE_THUMB if thumb else UC_MODE_ARM)
    engine.mem_map(0, 1 << 20)
    engine.mem_write(0x1000, code)
    engine.mem_write(0x4000, struct.pack("<256" + ("Q" if a64 else "I"), *range(256)))
    engine.mem_write(0x40, struct.pack("<I", 64))
    observed, halted = 0, False

    def instruction(_engine, address, _size, _data):
        nonlocal observed
        if not 0x1000 <= address < 0x1000 + len(code):
            raise ValueError(f"execution left the instruction array: {address:#x}")
        observed += 1

    def interrupt(uc, number, _data):
        nonlocal halted
        if number != 2:  # ARM/AArch64 software interrupt (SVC)
            raise ValueError(f"unexpected guest interrupt: {number}")
        halted = True
        uc.emu_stop()

    engine.hook_add(UC_HOOK_CODE, instruction)
    engine.hook_add(UC_HOOK_INTR, interrupt)
    # Both limits detect a corrupted/nonterminating guest instead of hanging CI.
    engine.emu_start(0x1001 if thumb else 0x1000, 0x1000 + len(code), timeout=30_000_000, count=10_000_000)
    if not halted:
        raise ValueError("guest did not terminate with SVC within the instruction/time limit")
    result = engine.reg_read(UC_ARM64_REG_X0 if a64 else UC_ARM_REG_R0)
    if result != spec.expected_result:
        raise ValueError(f"guest result: {result}, expected {spec.expected_result}")
    if observed != declared_count:
        raise ValueError(f"declared {declared_count} instructions, independently executed {observed}")
    return observed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    for spec in WORKLOADS:
        source = (args.source_root / "tests" / "hosts" / f"{spec.host}.cpp").read_text()
        try:
            count = verify_workload(*load_workload(source, spec), spec)
        except Exception as error:
            raise SystemExit(f"FAIL instruction count {spec.host}/{spec.code}: {error}") from error
        print(f"PASS instruction count {spec.host}/{spec.code}: {count}", flush=True)


if __name__ == "__main__":
    main()
