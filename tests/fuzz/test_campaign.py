"""Campaign control-flow tests; no Unicorn, Capstone or compiled hosts needed.

The real A32/A64 main functions run against fixed oracle acceptance and host
status streams. This checks gates, accounting and CLI errors, not ISA semantics.
"""

import argparse
from contextlib import ExitStack, redirect_stderr, redirect_stdout
import importlib.util
import io
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, patch

from campaign import coverage_failure, generation_failure, positive_int, unsupported_rate


def load_campaign(architecture):
    path = Path(__file__).resolve().parents[1] / architecture / "fuzz.py"
    spec = importlib.util.spec_from_file_location(f"campaign_test_{architecture}", path)
    module = importlib.util.module_from_spec(spec)
    # Import-time register IDs and disassemblers are irrelevant to control flow.
    # Stub them even on developer machines where the real packages are installed.
    with patch.dict(sys.modules, {"capstone": MagicMock(), "unicorn": MagicMock()}):
        spec.loader.exec_module(module)
    return module


def run_main(architecture, options=(), *, accepted=None, statuses=None, mismatch=False):
    module = load_campaign(architecture)
    calls = 0

    def oracle(*_):
        nonlocal calls
        calls += 1
        return {} if accepted is None or calls <= accepted else None

    def host(argv, **_):
        count = len(Path(argv[2]).read_bytes())
        records = [0] * count if statuses is None else statuses
        assert len(records) == count
        Path(argv[3]).write_bytes(b"".join(bytes([status]) + bytes(module.RESULT_SIZE - 1) for status in records))
        return SimpleNamespace(returncode=0)

    stdout, stderr = io.StringIO(), io.StringIO()
    generated = (b"\x00\x00\x00\x00", []) if architecture == "a32" else [0]
    with ExitStack() as stack:
        stack.enter_context(patch.object(sys, "argv", ["fuzz.py", "--count", "10", *options]))
        stack.enter_context(patch.object(module, "generate", return_value=generated))
        stack.enter_context(patch.object(module, "random_state", return_value={}))
        stack.enter_context(patch.object(module, "run_unicorn", side_effect=oracle))
        stack.enter_context(patch.object(module, "pack_case", return_value=b"x"))
        stack.enter_context(patch.object(module, "unpack_result", side_effect=lambda blob: (blob[0], {})))
        stack.enter_context(patch.object(module, "diff_states", return_value=["deliberate mismatch"] if mismatch else []))
        runner = stack.enter_context(patch.object(module.subprocess, "run", side_effect=host))
        stack.enter_context(redirect_stdout(stdout))
        stack.enter_context(redirect_stderr(stderr))
        try:
            code = module.main()
        except SystemExit as error:
            code = error.code
    return code, stdout.getvalue(), stderr.getvalue(), runner.call_count


class CampaignGateTests(unittest.TestCase):
    def test_cli_rejects_nonpositive_counts_lengths_and_invalid_rates(self):
        for architecture in ("a32", "a64"):
            for option, value in [("--count", "0"), ("--count", "-1"), ("--count", "abc"),
                                  ("--len", "0"), ("--max-unsupported-rate", "-0.01"),
                                  ("--max-unsupported-rate", "1.01"), ("--max-unsupported-rate", "nan"),
                                  ("--max-unsupported-rate", "inf"), ("--max-unsupported-rate", "text")]:
                with self.subTest(architecture=architecture, option=option, value=value):
                    code, _, error, hosts = run_main(architecture, [option, value])
                    self.assertEqual(code, 2)
                    self.assertIn(option, error)
                    self.assertEqual(hosts, 0)

    def test_empty_or_incomplete_generation_fails_before_launching_host(self):
        for architecture in ("a32", "a64"):
            for accepted in (0, 1, 9):
                with self.subTest(architecture=architecture, accepted=accepted):
                    code, output, _, hosts = run_main(architecture, accepted=accepted)
                    self.assertEqual(code, 1)
                    self.assertIn(f"cases={accepted}", output)
                    self.assertIn("compared=0", output)
                    self.assertIn("FAIL coverage:", output)
                    self.assertEqual(hosts, 0)

    def test_explicit_words_allow_partial_oracle_acceptance_but_never_empty(self):
        for architecture in ("a32", "a64"):
            with self.subTest(architecture=architecture):
                code, output, _, hosts = run_main(architecture, ["--word", "0"], accepted=1)
                self.assertEqual((code, hosts), (0, 1))
                self.assertIn("cases=1", output)
                self.assertIn("compared=1", output)
                code, output, _, hosts = run_main(architecture, ["--word", "0"], accepted=0)
                self.assertEqual((code, hosts), (1, 0))
                self.assertIn("no oracle-accepted cases", output)

    def test_all_unsupported_fails_even_with_permissive_limit(self):
        for architecture in ("a32", "a64"):
            with self.subTest(architecture=architecture):
                code, output, _, hosts = run_main(architecture, ["--max-unsupported-rate", "1"], statuses=[1] * 10)
                self.assertEqual((code, hosts), (1, 1))
                self.assertIn("compared=0 unsupported=10 mismatches=0", output)
                self.assertIn("no cases were compared", output)

    def test_threshold_is_inclusive_and_excess_is_failure(self):
        for architecture in ("a32", "a64"):
            for count, limit, expected in [(0, "0", 0), (3, "0.30", 0), (3, "0.299999", 1), (4, "0.30", 1)]:
                with self.subTest(architecture=architecture, count=count, limit=limit):
                    code, output, _, _ = run_main(architecture, ["--max-unsupported-rate", limit],
                                                  statuses=[1] * count + [0] * (10 - count))
                    self.assertEqual(code, expected)
                    self.assertIn(f"compared={10 - count} unsupported={count} mismatches=0", output)
                    self.assertEqual("exceeds --max-unsupported-rate" in output, bool(expected))

    def test_mismatches_remain_failures_below_unsupported_limit(self):
        for architecture in ("a32", "a64"):
            with self.subTest(architecture=architecture):
                code, output, _, _ = run_main(architecture, ["--show", "0"], mismatch=True)
                self.assertEqual(code, 1)
                self.assertIn("compared=10 unsupported=0 mismatches=10", output)

    def test_dependency_free_helpers_cover_denominator_and_boundary(self):
        self.assertIsNotNone(generation_failure(10, 0, True))
        self.assertIsNone(generation_failure(10, 1, True))
        self.assertIsNotNone(generation_failure(10, 9))
        self.assertIsNone(generation_failure(10, 10))
        self.assertIsNotNone(coverage_failure(0, 0, 1))
        self.assertIsNotNone(coverage_failure(10, 10, 1))
        self.assertIsNone(coverage_failure(1000, 300, 0.30))
        self.assertIsNotNone(coverage_failure(1000, 301, 0.30))
        self.assertEqual(positive_int("1"), 1)
        for value in ("0", "1", "0.30"):
            self.assertEqual(unsupported_rate(value), float(value))
        for value in ("0", "-1", "1.5"):
            with self.assertRaises(argparse.ArgumentTypeError):
                positive_int(value)


if __name__ == "__main__":
    unittest.main()
