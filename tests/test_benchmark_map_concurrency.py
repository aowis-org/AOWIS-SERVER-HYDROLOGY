#!/usr/bin/env python3
"""Offline Map concurrency probe tests using an actual threaded loopback server."""

import argparse
import http.server
import pathlib
import struct
import sys
import threading
import time
import unittest
import urllib.parse
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
import benchmark_map_concurrency as probe


JOB = {
    "state": "completed", "dataset": "copernicus-glo30",
    "columns": 4, "rows": 4,
    "west": 8.0, "north": 50.0,
    "step_lon_deg": 0.0001, "step_lat_deg": 0.0001,
}
JOB_ID = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"


class MapHandler(http.server.BaseHTTPRequestHandler):
    lock = threading.Lock()
    active = 0
    max_active = 0
    drift = False
    request_seq = 0

    def do_GET(self):
        fields = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
        with self.lock:
            type(self).active += 1
            type(self).request_seq += 1
            request_seq = type(self).request_seq
            type(self).max_active = max(type(self).max_active, type(self).active)
        try:
            time.sleep(0.025)
            columns, rows = int(fields["columns"][0]), int(fields["rows"][0])
            seed = float(fields["west"][0]) + float(fields["north"][0])
            if type(self).drift:
                seed += request_seq / 10
            data = b"".join(struct.pack("<f", seed + (i % 13)) for i in range(columns * rows))
            self.send_response(200)
            self.send_header("Content-Type", "application/vnd.aowis.dem-f32le")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        finally:
            with self.lock:
                type(self).active -= 1

    def log_message(self, *_args):
        pass


class ConcurrencyProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), MapHandler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.url = "http://127.0.0.1:" + str(cls.server.server_port)

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)

    def setUp(self):
        MapHandler.max_active = 0
        MapHandler.active = 0
        MapHandler.drift = False
        MapHandler.request_seq = 0

    def args(self, *extra):
        return probe.build_parser().parse_args([
            "--existing-job", JOB_ID,
            "--map-url", self.url,
            "--source-patch-size", "2",
            "--rounds", "2", "--warmup-rounds", "0",
            *extra,
        ])

    def test_parallel_map_grid_requests_are_byte_identical(self):
        with mock.patch.object(probe.single, "http_json", return_value=(JOB, 0.3)):
            report = probe.run(self.args())
        self.assertEqual(report["patch_count"], 4)
        self.assertEqual(report["response_bytes_per_pass"], 4 * 4 * 4)
        self.assertEqual(len(report["source_patches_sha256"]), 4)
        self.assertEqual([entry["workers"] for entry in report["results"]], [1, 2, 4])
        self.assertTrue(all(entry["wall"]["count"] == 2 for entry in report["results"]))
        self.assertGreaterEqual(MapHandler.max_active, 2)

    def test_data_drift_fails_without_report(self):
        MapHandler.drift = True
        with mock.patch.object(probe.single, "http_json", return_value=(JOB, 0.1)):
            with self.assertRaisesRegex(probe.single.BenchmarkError, "different DEM bytes"):
                probe.run(self.args("--rounds", "1"))

    def test_invalid_job_is_rejected_before_map_requests(self):
        for replacement in ({**JOB, "state": "queued"},
                            {**JOB, "rows": 9999},
                            {**JOB, "step_lon_deg": 0},
                            {**JOB, "columns": -1}):
            with self.subTest(replacement=replacement):
                with mock.patch.object(probe.single, "http_json", return_value=(replacement, 0)):
                    with mock.patch.object(probe, "probe_round") as read:
                        with self.assertRaises(probe.single.BenchmarkError):
                            probe.run(self.args())
                        read.assert_not_called()

    def test_worker_limits_and_preflight_budget(self):
        for bad in ("2,3", "1,1,2", "1,9", "1,garbage", ""):
            with self.assertRaises(argparse.ArgumentTypeError):
                probe.parse_workers(bad)
        self.assertEqual(probe.parse_workers("1,2,4"), [1, 2, 4])
        with mock.patch.object(probe.single, "http_json", return_value=(JOB, 0)):
            with self.assertRaisesRegex(probe.single.BenchmarkError, "exceeds"):
                probe.run(self.args("--max-cells", "10"))


if __name__ == "__main__":
    unittest.main()
