#!/usr/bin/env python3
"""Unit tests for resource scaling runner without a C++ compiler."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("benchmark_resource_scaling", ROOT / "tools/benchmark_resource_scaling.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class ResourceScalingTests(unittest.TestCase):
    def test_size_validation(self):
        for valid in ("16x16", "256x512", "5000x2000"):
            self.assertEqual(len(bench.size_arg(valid)), 2)
        for invalid in ("not-a-size", "0x5", "6000x20", "5000x5000", "10x12x13"):
            with self.subTest(size=invalid):
                with self.assertRaises(Exception):
                    bench.size_arg(invalid)

    def test_summary_missing_memory_is_unknown(self):
        samples = [{"measured_stages_total_ms": 100, "peak_process_rss_bytes": None,
                    "elevation_fingerprint_fnv64": "abcd"}]
        result = bench.summarize(samples, (16, 16))
        self.assertIsNone(result["peak_process_rss_bytes"])
        self.assertIsNone(result["observed_peak_rss_bytes_per_cell"])

    def test_summary_distributions(self):
        samples = [{"measured_stages_total_ms": t, "peak_process_rss_bytes": m,
                    "elevation_fingerprint_fnv64": "same"}
                   for t, m in [(20, 4096), (10, 2048), (15, 3072)]]
        result = bench.summarize(samples, (16, 16))
        self.assertEqual(result["stage_time_ms"]["median"], 15)
        self.assertEqual(result["peak_process_rss_bytes"]["median"], 3072)
        self.assertEqual(result["observed_peak_rss_bytes_per_cell"], 12)
        self.assertEqual(result["identities"], ["same"])

    def test_runner_rejects_unavailable_executable(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(SystemExit) as error:
                bench.main(["--executable", str(Path(directory) / "missing")])
            self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
