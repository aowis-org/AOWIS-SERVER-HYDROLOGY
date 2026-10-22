#!/usr/bin/env python3
"""Offline protocol tests for tools/benchmark_map_http.py (no Qt, Map or network)."""

import contextlib
import http.server
import importlib.util
import io
import json
import math
import pathlib
import socketserver
import struct
import threading
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "tools" / "benchmark_map_http.py"
SPEC = importlib.util.spec_from_file_location("benchmark_map_http", SCRIPT)
benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmark)


class Handler(http.server.BaseHTTPRequestHandler):
    state = {"post_count": 0, "fail_parity": False, "map_key": None}
    elevations = struct.pack("<" + "f" * 16, *range(16))

    def log_message(self, *_):
        pass

    def send_json(self, document, code=200):
        data = json.dumps(document).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def send_binary(self, document, content_type):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(document)))
        self.end_headers()
        self.wfile.write(document)

    def do_POST(self):
        if self.path != "/v1/jobs":
            self.send_json({"error": "unknown"}, 404)
            return
        self.__class__.state["post_count"] += 1
        self.send_json({"id": "00000000-0000-4000-8000-000000000001", "state": "queued"}, 202)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/status":
            self.send_json({"status": "ok"})
        elif path == "/v1/jobs/00000000-0000-4000-8000-000000000001":
            self.send_json({"id": "00000000-0000-4000-8000-000000000001", "state": "completed", "columns": 4,
                            "rows": 4, "basin_count": 1, "conditioned_cells": 2, "dataset": "copernicus-glo30",
                            "vertical_datum": "EGM2008", "west": 8.0, "north": 50.0,
                            "step_lon_deg": 0.0004, "step_lat_deg": 0.0004})
        elif path.endswith("/raster/elevation"):
            from urllib.parse import parse_qs, urlsplit
            q = parse_qs(urlsplit(self.path).query)
            x, y, width, height = (int(q[k][0]) for k in ("x", "y", "width", "height"))
            source = [float(i) for i in range(16)]
            sample = [source[r*4+c] for r in range(y, y+height) for c in range(x, x+width)]
            self.send_binary(struct.pack("<" + "f" * len(sample), *sample), "application/vnd.aowis.raster-f32le")
        elif path.endswith("/basins"):
            self.send_json({"total": 1, "basins": [{"id": "basin"}]})
        elif path == "/terrain/v1/analysis/grid":
            from urllib.parse import parse_qs, urlsplit
            q = parse_qs(urlsplit(self.path).query)
            width, height = int(q["columns"][0]), int(q["rows"][0])
            west, north = float(q["west"][0]), float(q["north"][0])
            x = round((west - 8.0) / 0.0004)
            y = round((50.0 - north) / 0.0004)
            elevations = [float(r*4+c) for r in range(y, y+height) for c in range(x, x+width)]
            if self.__class__.state["fail_parity"]:
                elevations[width + 1] += 1
            self.__class__.state["map_key"] = self.headers.get("X-API-Key")
            self.send_binary(struct.pack("<"+"f"*len(elevations), *elevations), "application/vnd.aowis.dem-f32le")
        else:
            self.send_json({"error": "unknown"}, 404)


class ReusableServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


class LiveBenchmarkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ReusableServer(("127.0.0.1", 0), Handler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.url = f"http://127.0.0.1:{cls.server.server_port}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)

    def setUp(self):
        Handler.state = {"post_count": 0, "fail_parity": False, "map_key": None}

    def args(self, *more):
        return benchmark.build_parser().parse_args([
            "--hydrology-url", self.url, "--map-url", self.url,
            "--west", "8", "--south", "49.9984", "--east", "8.0016", "--north", "50",
            "--resolution-m", "30", "--max-cells", "100", "--window", "2",
            "--repeats", "2", "--poll-seconds", "0.001", *more,
        ])

    def test_live_job_and_parity(self):
        result = benchmark.run(self.args("--map-read-api-key", "secret-example"))
        self.assertEqual(result["cells"], 16)
        self.assertTrue(result["map_hydrology_parity"]["exact_bytes_equal"])
        self.assertEqual(result["elevation_window"]["count"], 2)
        self.assertEqual(Handler.state["post_count"], 1)
        self.assertEqual(Handler.state["map_key"], "secret-example")
        self.assertNotIn("secret-example", json.dumps(result))

    def test_existing_job_never_posts(self):
        result = benchmark.run(self.args("--existing-job", "00000000-0000-4000-8000-000000000001"))
        self.assertEqual(result["mode"], "existing_job")
        self.assertEqual(Handler.state["post_count"], 0)

    def test_preflight_cell_limit_before_job_submission(self):
        with self.assertRaisesRegex(benchmark.BenchmarkError, "exceeds --max-cells"):
            benchmark.run(self.args("--max-cells", "3"))
        self.assertEqual(Handler.state["post_count"], 0)

    def test_map_parity_failure_is_not_suppressed(self):
        Handler.state["fail_parity"] = True
        with self.assertRaisesRegex(benchmark.BenchmarkError, "parity failed"):
            benchmark.run(self.args())

    def test_native_pixel_boundaries_use_original_source_grid(self):
        # The small-window request used by the old benchmark could shift
        # latitude by one double ULP and cross a 1-arcsecond DEM pixel edge.
        metadata = {"columns": 47, "rows": 75, "west": 8.7, "north": 50.72,
                    "step_lon_deg": 0.02 / 47, "step_lat_deg": 0.02 / 75,
                    "dataset": "copernicus-glo30"}
        rect = {"x": 0, "y": 5, "width": 47, "height": 64}

        def native_sample(params):
            west, east = float(params["west"]), float(params["east"])
            north, south = float(params["north"]), float(params["south"])
            width, height = int(params["columns"]), int(params["rows"])
            dx, dy = (east - west) / width, (north - south) / height
            values = []
            for r in range(height):
                lat = north - (r + 0.5) * dy
                native_r = math.floor((51.0 - lat) * 3600)
                for c in range(width):
                    lon = west + (c + 0.5) * dx
                    native_c = math.floor((lon - 8.0) * 3600)
                    values.append(float(native_r * 4000 + native_c))
            return struct.pack("<" + "f" * len(values), *values)

        def fake_request(_base, _path, _timeout, **kwargs):
            payload = native_sample(kwargs["parameters"])
            return payload, "application/vnd.aowis.dem-f32le", 0.5

        original_params = next(benchmark.map_acquisition_patches(metadata, rect, 256))[4]
        source = native_sample(original_params)
        expected = source[rect["y"] * 47 * 4:(rect["y"] + rect["height"]) * 47 * 4]
        with mock.patch.object(benchmark, "http_request", side_effect=fake_request):
            actual, _, bytes_read, requests = benchmark.fetch_original_map_window(
                "http://map", 10, {}, metadata, rect, 256)
        self.assertEqual(actual, expected)
        self.assertEqual(bytes_read, 47 * 75 * 4)
        self.assertEqual(requests, 1)

        # Verify the regression is meaningful: requesting the cropped window
        # as a new Map grid actually selects some different native pixels.
        naive_north = metadata["north"] - rect["y"] * metadata["step_lat_deg"]
        naive_west = metadata["west"]
        naive_params = dict(original_params, west=format(naive_west, ".17g"),
                            east=format(naive_west + 47 * metadata["step_lon_deg"], ".17g"),
                            north=format(naive_north, ".17g"),
                            south=format(naive_north - 64 * metadata["step_lat_deg"], ".17g"),
                            columns=47, rows=64)
        naive = native_sample(naive_params)
        mismatches, _, _ = benchmark.parity_differences(naive, expected, rect, 0.0001)
        self.assertGreater(mismatches, 0)

    def test_source_patch_window_crosses_four_patches(self):
        metadata = {"columns": 7, "rows": 6, "west": 8.0, "north": 50.0,
                    "step_lon_deg": 0.001, "step_lat_deg": 0.001,
                    "dataset": "copernicus-glo30"}
        rect = {"x": 2, "y": 1, "width": 4, "height": 4}
        calls = []

        def fake_request(_base, _path, _timeout, **kwargs):
            params = kwargs["parameters"]
            calls.append(params)
            px = round((float(params["west"]) - 8.0) / 0.001)
            py = round((50.0 - float(params["north"])) / 0.001)
            width, height = int(params["columns"]), int(params["rows"])
            data = [float((py + r) * 7 + px + c)
                    for r in range(height) for c in range(width)]
            return (struct.pack("<" + "f" * len(data), *data),
                    "application/vnd.aowis.dem-f32le", 1.0)

        with mock.patch.object(benchmark, "http_request", side_effect=fake_request):
            data, elapsed, bytes_read, requests = benchmark.fetch_original_map_window(
                "http://map", 10, {}, metadata, rect, 3)
        actual = [value[0] for value in struct.iter_unpack("<f", data)]
        expected = [float(r * 7 + c) for r in range(1, 5) for c in range(2, 6)]
        self.assertEqual(actual, expected)
        self.assertEqual(requests, 4)
        self.assertEqual(len(calls), 4)
        self.assertEqual(elapsed, 4.0)
        self.assertEqual(bytes_read, (9 + 9 + 9 + 9) * 4)

    def test_url_validation(self):
        for url in ("file:///tmp/x", "http://user:password@localhost", "http://localhost/?key=private"):
            with self.assertRaises(benchmark.BenchmarkError):
                benchmark.endpoint_url(url, "/status")

    def test_read_window_and_timing_statistics(self):
        self.assertEqual(benchmark.timed_summary([10, 5, 3])["median_ms"], 5)
        self.assertEqual(benchmark.estimated_grid((8, 49.9984, 8.0016, 50), 30), (4, 6))


if __name__ == "__main__":
    unittest.main()
