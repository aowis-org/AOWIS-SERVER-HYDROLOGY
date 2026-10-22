#!/usr/bin/env python3
"""Offline repeated-job benchmark tests; no Qt or running services required."""

import copy
import pathlib
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
import benchmark_map_series as series


def report(index):
    return {
        "job_id": f"new-job-{index}",
        "raster_columns": 4, "raster_rows": 4,
        "source_dataset": "copernicus-glo30",
        "basin_count": 3, "conditioned_cells": 5,
        "elevation_window": {"sha256": "same-elevation-bytes"},
        "job_wait_s": 0.4 + index * 0.01,
        "server_stage_timings_ms": {
            "terrain_fetch": 100.0 + index,
            "terrain_validation": 1.0,
            "d8_analysis": 12.0,
            "raster_persistence": 4.0,
            "depression_hierarchy": 20.0,
            "basin_persistence": 2.0,
            "total_worker": 150.0 + index,
        },
        "server_terrain_fetch_details": {
            "transport": "map_http", "patch_count": 2,
            "response_bytes": 64, "map_request_ms": 90.0 + index,
            "raster_copy_ms": 2.0,
            "first_request_ms": 45.0 + index,
            "fastest_request_ms": 44.0,
            "slowest_request_ms": 46.0 + index,
        },
        "map_hydrology_parity": {"cells_outside_tolerance": 0,
                                 "exact_bytes_equal": True},
    }


class RepeatedBenchmarkTests(unittest.TestCase):
    def args(self, *extra):
        return series.build_parser().parse_args([
            "--west", "8", "--south", "49.9984", "--east", "8.0016", "--north", "50",
            "--resolution-m", "30", "--max-cells", "100", *extra,
        ])

    def test_distributions_and_provenance(self):
        summary = series.summarize([report(1), report(2), report(3), report(4), report(5)], 1)
        self.assertEqual(summary["measured_jobs"], 5)
        self.assertEqual(summary["patch_count_per_job"], 2)
        self.assertEqual(summary["terrain_transport"], "map_http")
        self.assertEqual(summary["timing_distributions_ms"]["terrain_fetch"]["median"], 103.0)
        self.assertEqual(summary["timing_distributions_ms"]["terrain_fetch"]["p90_nearest_rank"], 105.0)
        self.assertEqual(summary["timing_distributions_ms"]["fetch_unattributed_ms"]["median"], 8.0)
        self.assertAlmostEqual(summary["share_distributions_pct"]["request_of_fetch_pct"]["median"],
                               round(93/103 * 100, 3))
        self.assertEqual(len(summary["jobs"]), 5)

    def test_warmup_runs_excluded_and_fresh_jobs(self):
        args = self.args("--runs", "3", "--warmup-runs", "1")
        calls = []
        with mock.patch.object(series.single, "run", side_effect=[report(i) for i in range(4)]) as worker:
            summary = series.run_series(args, lambda i, n, warmup: calls.append((i, n, warmup)))
        self.assertEqual(worker.call_count, 4)
        self.assertEqual(calls, [(1, 4, True), (2, 4, False), (3, 4, False), (4, 4, False)])
        self.assertEqual(summary["warmup_jobs_not_reported"], 1)
        self.assertEqual([job["job_id"] for job in summary["jobs"]],
                         ["new-job-1", "new-job-2", "new-job-3"])
        self.assertEqual(args.repeats, 1)

    def test_rejects_source_output_drift(self):
        changed = copy.deepcopy(report(2))
        changed["elevation_window"]["sha256"] = "drifted"
        with self.assertRaisesRegex(series.single.BenchmarkError, "output changed"):
            series.summarize([report(1), changed])
        with self.assertRaisesRegex(series.single.BenchmarkError, "reused job ID"):
            series.summarize([report(1), report(1)])

    def test_rejects_missing_or_inconsistent_instrumentation(self):
        missing = report(2)
        del missing["server_terrain_fetch_details"]
        with self.assertRaisesRegex(series.single.BenchmarkError, "diagnostics missing"):
            series.summarize([report(1), missing])
        bad = report(2)
        bad["server_terrain_fetch_details"]["map_request_ms"] = 500.0
        with self.assertRaisesRegex(series.single.BenchmarkError, "inconsistent terrain fetch"):
            series.summarize([report(1), bad])

    def test_overlapping_request_accounting(self):
        first = report(1)
        second = report(2)
        for item in (first, second):
            details = item["server_terrain_fetch_details"]
            details["peak_in_flight"] = 2
            details["map_request_wall_ms"] = 50.0
            details["map_request_ms"] = 145.0
        summary = series.summarize([first, second])
        self.assertEqual(summary["timing_distributions_ms"]["map_request_wall_ms"]["median"], 50.0)
        self.assertEqual(summary["timing_distributions_ms"]["peak_in_flight"]["median"], 2.0)
        self.assertEqual(summary["timing_distributions_ms"]["fetch_unattributed_ms"]["median"], 51.5)
        self.assertGreater(summary["share_distributions_pct"]["overlap_ratio"]["median"], 2.0)
        broken = report(3)
        broken["server_terrain_fetch_details"]["peak_in_flight"] = 2
        broken["server_terrain_fetch_details"]["map_request_wall_ms"] = 200.0
        with self.assertRaisesRegex(series.single.BenchmarkError, "inconsistent terrain fetch"):
            series.summarize([first, broken])

    def test_preflight_rejects_without_post(self):
        for opts in (("--max-cells", "2"), ("--runs", "21"),
                     ("--existing-job", "fake-id"), ("--warmup-runs", "6")):
            with self.subTest(opts=opts):
                args = self.args(*opts)
                with mock.patch.object(series.single, "run") as worker:
                    with self.assertRaises(series.single.BenchmarkError):
                        series.run_series(args)
                    worker.assert_not_called()

    def test_run_failure_identifies_iteration(self):
        args = self.args("--runs", "3")
        with mock.patch.object(series.single, "run", side_effect=[
            report(0), series.single.BenchmarkError("connection reset"), report(2)]):
            with self.assertRaisesRegex(series.single.BenchmarkError, "job 2/3 failed: connection reset"):
                series.run_series(args)


if __name__ == "__main__":
    unittest.main()
