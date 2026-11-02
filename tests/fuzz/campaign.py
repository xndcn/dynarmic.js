"""Dependency-free coverage gates shared by the differential fuzz campaigns."""

import argparse
import math


def positive_int(text):
    try:
        value = int(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a positive integer") from error
    if value <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return value


def unsupported_rate(text):
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a finite fraction between 0 and 1") from error
    if not math.isfinite(value) or not 0 <= value <= 1:
        raise argparse.ArgumentTypeError("must be a finite fraction between 0 and 1")
    return value


def generation_failure(requested, generated, explicit_word=False):
    if generated == 0:
        return "no oracle-accepted cases were generated"
    if generated < requested and not explicit_word:
        return f"generated only {generated} of {requested} requested oracle-accepted cases"
    return None


def coverage_failure(cases, unsupported, maximum):
    # A permissive maximum never permits zero actual state comparisons.
    compared = cases - unsupported
    if cases == 0 or compared <= 0:
        return "no cases were compared"
    if unsupported / cases > maximum:
        return (f"unsupported rate {unsupported}/{cases}={unsupported / cases:.6f} "
                f"exceeds --max-unsupported-rate={maximum:g}")
    return None
