#!/usr/bin/env python3
"""Offline simultaneous Hydrology job benchmark contract tests."""

import copy
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import pathlib
import struct
import sys
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
import benchmark_map_jobs as jobs


class Fixture:
    def __init__(self):
        self.lock = threading.Lock()
        self.created = 0
        self.server = None
        self.thread = None
        self.identical = True
        self.started = []
        self.invalid_queue = False
        self.invalid_map_field = False

    def __enter__(self):
        fixture = self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def send_json(self, document):
                self.send_blob(json.dumps(document).encode("utf-8"), "application/json")

            def send_blob(self, payload, mime):
                self.send_response(200)
                self.send_header("Content-Type", mime)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def metadata(self, ident, full):
                result = {"id": ident, "state": "completed" if full else "queued"}
                if full:
                    result.update({"columns": 4, "rows": 4, "dataset": "copernicus-glo30",
                        "basin_count": 3 if fixture.identical else int(ident.split("-")[-1]),
                        "conditioned_cells": 2,
                        "stage_timings_ms": {"terrain_fetch": 20., "d8_analysis": 10.,
                            "depression_hierarchy": 10., "total_worker": 50.,
                            "queue_wait": -1.0 if fixture.invalid_queue else 1.0},
                        "terrain_fetch_details": {
                            "peak_in_flight": 4, "transport": "map_http",
                            "patch_count": 2, "response_bytes": 64,
                            "map_request_ms": 15.0, "map_request_wall_ms": -1.0 if fixture.invalid_map_field else 12.0,
                            "raster_copy_ms": 0.2, "first_request_ms": 8.0,
                            "fastest_request_ms": 7.0, "slowest_request_ms": 8.0}})
                return result

            def do_POST(self):
                if self.path != "/v1/jobs":
                    self.send_error(404)
                    return
                length = int(self.headers.get("Content-Length", "0"))
                payload = json.loads(self.rfile.read(length))
                if not all(key in payload for key in ("west", "south", "east", "north", "dataset")):
                    self.send_error(400)
                    return
                with fixture.lock:
                    fixture.created += 1
                    ident = f"job-{fixture.created}"
                    fixture.started.append((ident, time.perf_counter()))
                self.send_json(self.metadata(ident, False))

            def do_GET(self):
                if self.path == "/status":
                    self.send_json({"status": "ok", "max_concurrent_analyses": 4,
                                    "map_requests_per_analysis": 2})
                    return
                path, _, query = self.path.partition("?")
                chunks = path.strip("/").split("/")
                if len(chunks) >= 3 and chunks[:2] == ["v1", "jobs"]:
                    ident = chunks[2]
                    with fixture.lock:
                        started = dict(fixture.started).get(ident)
                    if started is None:
                        self.send_error(404)
                        return
                    if len(chunks) == 3:
                        self.send_json(self.metadata(ident, time.perf_counter() - started >= 0.06))
                        return
                    if chunks[3:] == ["raster", "elevation"]:
                        # All jobs contain the same deterministic 4x4 window.
                        self.send_blob(struct.pack("<16f", *range(16)),
                                       "application/vnd.aowis.raster-f32le")
                        return
                self.send_error(404)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        return f"http://127.0.0.1:{self.server.server_port}"

    def __exit__(self, *_):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


def minimal_args(*options):
    return jobs.build_parser().parse_args([
        "--west", "8", "--south", "49.9984", "--east", "8.0016", "--north", "50",
        "--resolution-m", "30", "--max-cells", "100", "--jobs", "1,2",
        "--rounds", "2", "--poll-seconds", "0.01", *options])


class ConcurrentJobTests(unittest.TestCase):
    def test_rejects_bad_levels(self):
        for bad in ("2,3", "1,1", "1,5", "", "1,no", "1,2,3,4,4"):
            with self.subTest(bad=bad), self.assertRaises(jobs.argparse.ArgumentTypeError):
                jobs.levels_arg(bad)
        self.assertEqual(jobs.levels_arg("4,1,2"), [4, 1, 2])

    def test_preflight_refuses_excessive_work_before_http(self):
        options = (("--max-cells", "2"), ("--max-batch-cells", "2"),
                   ("--max-total-cells", "2"), ("--warmup-rounds", "3"),
                   ("--rounds", "6"), ("--existing-job", "other"),
                   ("--rss-poll-seconds", "0.001"),
                   ("--rss-settle-seconds", "11"), ("--repeats", "2"))
        for choice in options:
            with self.subTest(choice=choice):
                args = minimal_args(*choice)
                with mock.patch.object(jobs.single, "http_json") as request:
                    with self.assertRaises(jobs.single.BenchmarkError):
                        jobs.benchmark(args)
                    request.assert_not_called()

    def test_end_to_end_concurrent_jobs_and_post_wave_verification(self):
        with Fixture() as url:
            args = minimal_args("--hydrology-url", url)
            report = jobs.benchmark(args)
            self.assertEqual(report["total_jobs_created"], 6)
            self.assertEqual([group["jobs"] for group in report["results"]], [1, 2])
            self.assertEqual(report["raster_columns"], 4)
            self.assertEqual(report["dataset"], "copernicus-glo30")
            self.assertEqual(len(report["measured_waves"]), 4)
            self.assertEqual(len({job["job_id"] for wave in report["measured_waves"]
                for job in wave["jobs_details"]}), 6)
            self.assertGreater(report["results"][1]["relative_throughput_vs_single"], 1.0)
            self.assertTrue(all(wave["rss"] is None for wave in report["measured_waves"]))
            self.assertTrue(all(wave["jobs_details"][0]["elevation_window_sha256"] ==
                report["elevation_window_sha256"] for wave in report["measured_waves"]))
            self.assertEqual(report["results"][1]["queue_wait_ms"]["median"], 1.0)
            self.assertEqual(report["server_settings"], {
                "max_concurrent_analyses": 4, "map_requests_per_analysis": 2})
            self.assertEqual(report["results"][1]["terrain_fetch_details"]["map_request_wall_ms"]["median"], 12.0)
            self.assertEqual(report["results"][1]["terrain_fetch_details"]["map_request_ms"]["median"], 15.0)
            self.assertEqual(report["results"][1]["terrain_fetch_details"]["patch_count"]["median"], 2)
            self.assertEqual(report["measured_waves"][0]["jobs_details"][0]["terrain_fetch_details"]["transport"], "map_http")

    def test_invalid_queue_wait_is_rejected(self):
        fixture = Fixture()
        fixture.invalid_queue = True
        with fixture as url:
            args = minimal_args("--hydrology-url", url)
            with self.assertRaisesRegex(jobs.single.BenchmarkError, "invalid timing queue_wait"):
                jobs.benchmark(args)

    def test_invalid_map_diagnostic_is_rejected(self):
        fixture = Fixture()
        fixture.invalid_map_field = True
        with fixture as url:
            args = minimal_args("--hydrology-url", url)
            with self.assertRaisesRegex(jobs.single.BenchmarkError, "invalid Map diagnostic map_request_wall_ms"):
                jobs.benchmark(args)

    def test_missing_optional_map_diagnostics_are_supported(self):
        baseline = (4, 4, "dataset", 3, 2, "digest")
        details = {"latency_ms": 100.0, "queue_wait_ms": 0.1,
                   "terrain_peak_in_flight": 4, "terrain_fetch_details": {},
                   "stage_timings_ms": {key: 10.0 for key in jobs.STAGES}}
        wave = {"jobs": 1, "round": 1, "identity": baseline,
                "completion_wall_ms": 100.0,
                "completed_jobs_per_second": 10.0,
                "jobs_details": [details], "rss": None}
        group = jobs.summarize([wave], [1], 1, 0, 16)["results"][0]
        self.assertIsNone(group["terrain_fetch_details"]["map_request_wall_ms"])
        self.assertIsNone(group["terrain_fetch_details"]["transport"])

    def test_output_drift_is_rejected(self):
        fixture = Fixture()
        fixture.identical = False
        with fixture as url:
            args = minimal_args("--hydrology-url", url)
            with self.assertRaisesRegex(jobs.single.BenchmarkError, "nonidentical analysis output"):
                jobs.benchmark(args)

    def test_bad_worker_diagnostics_rejected(self):
        args = minimal_args()
        barrier = threading.Barrier(1)
        with mock.patch.object(jobs.single, "http_json", side_effect=[
            ({"id": "job-1", "state": "completed", "columns": 4, "rows": 4,
              "stage_timings_ms": {"total_worker": 0},
              "terrain_fetch_details": {"peak_in_flight": 1}}, 1.0)]):
            with self.assertRaisesRegex(jobs.single.BenchmarkError, "invalid timing"):
                jobs.submit_and_poll(args, barrier, 0)

    def test_linux_rss_sampler_and_missing_pid(self):
        with mock.patch.object(jobs, "read_rss_kib", return_value=1024) as getter:
            with jobs.RssSampler({"map": 1234}, 0.01) as sampler:
                time.sleep(0.025)
            summary = sampler.summary()
        self.assertGreaterEqual(getter.call_count, 1)
        self.assertGreaterEqual(summary["map"]["samples"], 1)
        self.assertEqual(summary["map"]["peak_increase_kib"], 0)
        with self.assertRaises(jobs.single.BenchmarkError):
            jobs.read_rss_kib(999999999)

    def test_post_wave_rss_does_not_extend_completion_wall(self):
        with Fixture() as url:
            args = minimal_args("--hydrology-url", url, "--jobs", "1", "--rounds", "1",
                                "--hydrology-pid", "1234", "--rss-settle-seconds", "0.2")
            with mock.patch.object(jobs, "read_rss_kib", return_value=2048) as reader:
                begin = time.perf_counter()
                report = jobs.benchmark(args)
                elapsed_ms = (time.perf_counter() - begin) * 1000
        self.assertGreaterEqual(reader.call_count, 2)
        wave = report["measured_waves"][0]
        rss = wave["rss"]["hydrology"]
        self.assertEqual(rss["settled_kib"], 2048)
        self.assertEqual(rss["settled_change_kib"], 0)
        self.assertGreaterEqual(elapsed_ms - wave["completion_wall_ms"], 180)
        group = report["results"][0]["rss_kib"]["hydrology"]
        self.assertEqual(group["settled_kib"]["median"], 2048)
        self.assertEqual(group["settled_change_kib"]["median"], 0)

    def test_rss_statistical_summary_rejects_mixed_sampling(self):
        baseline = (4, 4, "dataset", 3, 2, "digest")
        details = {"latency_ms": 100.0, "queue_wait_ms": 0.1,
                   "terrain_peak_in_flight": 4,
                   "stage_timings_ms": {key: 10.0 for key in jobs.STAGES}}
        def wave(round_no, rss):
            return {"jobs": 1, "round": round_no, "identity": baseline,
                    "completion_wall_ms": 100.0,
                    "completed_jobs_per_second": 10.0,
                    "jobs_details": [details], "rss": rss}
        values = {"initial_kib": 50, "peak_kib": 100,
                  "peak_increase_kib": 50, "settled_kib": 60,
                  "settled_change_kib": 10}
        with self.assertRaisesRegex(jobs.single.BenchmarkError, "mixed RSS"):
            jobs.summarize([wave(1, None), wave(2, {"hydrology": values})],
                           [1], 2, 0, 16)
        group = jobs.summarize([wave(1, {"hydrology": values}),
                                wave(2, {"hydrology": values})],
                               [1], 2, 0, 16)["results"][0]
        self.assertEqual(group["rss_kib"]["hydrology"]["peak_kib"]["median"], 100)

    def test_completed_verify_mime_and_hash(self):
        args = minimal_args()
        base = {"job_id": "one", "columns": 4, "rows": 4,
                "dataset": "copernicus-glo30", "basin_count": 2,
                "conditioned_cells": 1, "metadata": {}}
        first = copy.deepcopy(base)
        second = copy.deepcopy(base)
        second["job_id"] = "two"
        with mock.patch.object(jobs.single, "http_request", side_effect=[
            (b"b" * 64, "application/vnd.aowis.raster-f32le", 0.1),
            (b"c" * 64, "application/vnd.aowis.raster-f32le", 0.1)]):
            with self.assertRaisesRegex(jobs.single.BenchmarkError, "nonidentical analysis output"):
                jobs.verify_wave(args, [first, second])
        with mock.patch.object(jobs.single, "http_request", return_value=(b"x", "text/plain", 0.1)):
            with self.assertRaisesRegex(jobs.single.BenchmarkError, "invalid persisted elevation"):
                jobs.verify_wave(args, [copy.deepcopy(base)])

    def test_summaries_reject_incomplete_and_bad_identity(self):
        baseline = (4, 4, "dataset", 3, 2, "digest")
        wave = {"jobs": 1, "identity": baseline, "completion_wall_ms": 100.0,
                "completed_jobs_per_second": 10.0, "jobs_details": [{
                    "latency_ms": 100.0, "terrain_peak_in_flight": 4,
                    "stage_timings_ms": {key: 10.0 for key in jobs.STAGES}}]}
        with self.assertRaisesRegex(jobs.single.BenchmarkError, "incomplete"):
            jobs.summarize([wave], [1, 2], 1, 0, 16)
        self.assertEqual(jobs.summarize([wave], [1], 1, 0, 16)
                         ["results"][0]["relative_throughput_vs_single"], 1.0)


if __name__ == "__main__":
    unittest.main()
