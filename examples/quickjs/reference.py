#!/usr/bin/env python3
"""Independent reference: integer LCG, trial division and high-precision pi sum."""
from decimal import Decimal, localcontext
import json
from math import isqrt


def reference(lcg_steps, sieve_limit, pi_steps):
    value = 12345
    for _ in range(lcg_steps):
        value = (value * 1664525 + 1013904223) % 2**32
        value ^= value >> 13
    primes = sum(all(n % divisor for divisor in range(2, isqrt(n) + 1))
                 for n in range(2, sieve_limit))
    with localcontext() as context:
        context.prec = 50
        total = sum((Decimal(1 if k % 2 == 0 else -1) / (2 * k + 1)
                     for k in range(pi_steps)), Decimal(0))
        pi = int(total * 4_000_000)
    return {"lcg": value, "sieve": primes, "pi": pi}


if __name__ == "__main__":
    results = {"light": reference(20_000, 8192, 20_000),
               "full": reference(200_000, 65536, 200_000)}
    assert results == {"light": {"lcg": 2778886796, "sieve": 1028, "pi": 3141542},
                       "full": {"lcg": 428440788, "sieve": 6542, "pi": 3141587}}
    print(json.dumps(results, indent=2))
