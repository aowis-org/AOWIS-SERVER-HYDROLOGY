#!/usr/bin/env python3
"""Black-box Hydrology HTTP contract regression against a deterministic Map mock.

The same tests must pass against both the Qt adapter and its future replacement.
No Map service, credentials, internet access, external datasets or Python packages
are required. Run through CTest or pass the built server executable explicitly.
"""

import http.server
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlsplit
from urllib.request import Request, urlopen


class MapFixture(http.server.BaseHTTPRequestHandler):
    """One small, strictly descending DEM; cell centres are supplied by Map."""

    def do_GET(self):
        uri = urlsplit(self.path)
        if uri.path != "/terrain/v1/analysis/grid":
            self.send_error(404)
            return
        query = parse_qs(uri.query)
        try:
            columns = int(query["columns"][0])
            rows = int(query["rows"][0])
            assert query["dataset"] == ["copernicus-glo30"]
            assert 1 <= rows <= 256 and 1 <= columns <= 256
        except (KeyError, ValueError, AssertionError):
            self.send_error(400)
            return
        release = getattr(self.server, "terrain_release", None)
        if release is not None and not release.wait(timeout=15):
            self.send_error(503, "Mock terrain request timed out")
            return
        values = [float(1000 - 10 * y - x)
                  for y in range(rows) for x in range(columns)]
        data = struct.pack("<" + "f" * len(values), *values)
        self.send_response(200)
        self.send_header("Content-Type", "application/vnd.aowis.dem-f32le")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *_args):
        pass


def free_local_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class HttpContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if len(sys.argv) != 2:
            raise RuntimeError("Usage: test_http_contract.py <aowis-server-hydrology>")
        cls.executable = Path(sys.argv[1]).resolve()
        if not cls.executable.is_file():
            raise RuntimeError(f"Hydrology executable missing: {cls.executable}")
        sys.argv[1:] = []

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="aowis-hydrology-http-contract-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.map_server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), MapFixture)
        self.map_thread = threading.Thread(target=self.map_server.serve_forever, daemon=True)
        self.map_thread.start()
        self.addCleanup(self.stop_map)
        self.port = free_local_port()
        self.url = f"http://127.0.0.1:{self.port}"
        self.config = self.root / "server.ini"
        self.config.write_text(
            "[server]\n"
            "listen_address=127.0.0.1\n"
            f"port={self.port}\n"
            "output_directory=analyses\n"
            "max_cells=10000\n"
            "max_concurrent_analyses=1\n"
            "[terrain]\n"
            "dataset=copernicus-glo30\n"
            f"map_url=http://127.0.0.1:{self.map_server.server_port}/\n"
            "request_timeout_ms=5000\n"
            "patch_size=256\n"
            "max_concurrent_requests=1\n",
            encoding="utf-8",
        )
        self.process = None
        self.log = None
        self.addCleanup(self.stop_server)
        self.start_server()

    def stop_map(self):
        self.map_server.shutdown()
        self.map_server.server_close()
        self.map_thread.join(timeout=3)

    def start_server(self):
        log_path = self.root / f"server-{time.monotonic_ns()}.log"
        self.log = log_path.open("wb")
        env = os.environ.copy()
        env["XDG_DATA_HOME"] = str(self.root / "xdg-data")
        self.process = subprocess.Popen(
            [str(self.executable), "--config", str(self.config)],
            stdout=self.log, stderr=subprocess.STDOUT, env=env,
        )
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                break
            try:
                status, _mime, value = self.call("GET", "/status", timeout=0.4)
                if status == 200 and value.get("status") == "ok":
                    return
            except (OSError, URLError, ValueError):
                pass
            time.sleep(0.05)
        self.stop_server()
        self.fail(f"Hydrology did not start: {log_path.read_text(errors='replace')}")

    def stop_server(self):
        if self.process is not None:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=3)
            self.process = None
        if self.log is not None:
            self.log.close()
            self.log = None

    def call(self, method, path, payload=None, timeout=4):
        body = None
        headers = {}
        if payload is not None:
            body = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
            headers["Content-Type"] = "application/json"
        request = Request(self.url + path, data=body, headers=headers, method=method)
        try:
            response = urlopen(request, timeout=timeout)
        except HTTPError as error:
            response = error
        with response:
            mime = response.headers.get("Content-Type", "").split(";", 1)[0]
            data = response.read()
            if mime == "application/json":
                data = json.loads(data)
            return response.status, mime, data

    def expect_json(self, status, method, path, payload=None):
        actual, mime, result = self.call(method, path, payload)
        self.assertEqual(actual, status, (method, path, result))
        self.assertEqual(mime, "application/json", (method, path))
        self.assertIsInstance(result, dict)
        return result

    def await_completed(self, job_id):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            job = self.expect_json(200, "GET", f"/v1/jobs/{job_id}")
            if job["state"] == "completed":
                return job
            if job["state"] in ("failed", "canceled"):
                self.fail(f"Analysis did not complete: {job}")
            time.sleep(0.05)
        self.fail(f"Analysis timed out: {job_id}")

    def submit(self):
        created = self.expect_json(202, "POST", "/v1/jobs", {
            "west": 8.0, "south": 50.0, "east": 8.004, "north": 50.004,
            "resolution_m": 30.0, "fill_depressions": False,
        })
        self.assertRegex(created["id"], r"^[0-9a-f-]{36}$")
        return self.await_completed(created["id"])

    def raster(self, job_id, layer, width=2, height=2):
        path = (f"/v1/jobs/{job_id}/raster/{layer}"
                f"?x=0&y=0&width={width}&height={height}")
        status, mime, data = self.call("GET", path)
        self.assertEqual(status, 200, (layer, data))
        self.assertTrue(mime.startswith("application/vnd.aowis.raster-"), mime)
        return mime, data

    def persisted_job_directories(self):
        """Count analysis UUID directories, not transient service scratch space.

        Hydrology can keep ephemeral task spools beside immutable analysis
        directories. A rejected submission must not create a new analysis
        directory, but scratch storage is not itself an admitted analysis.
        """
        jobs = set()
        for path in (self.root / "analyses").iterdir():
            if not path.is_dir():
                continue
            try:
                canonical = str(uuid.UUID(path.name))
            except ValueError:
                continue
            if path.name == canonical:
                jobs.add(path.name)
        return jobs

    def test_status_errors_and_request_size(self):
        status = self.expect_json(200, "GET", "/status")
        self.assertEqual(status["service"], "aowis-server-hydrology")
        self.assertEqual(status["max_concurrent_analyses"], 1)
        self.assertEqual(status["max_outstanding_analyses"], 8)
        self.assertEqual(status["terrain_jobs"]["outstanding"], 0)
        self.assertEqual(status["map_requests_per_analysis"], 1)
        self.assertEqual(self.expect_json(200, "GET", "/v1/jobs")["jobs"], [])
        self.assertEqual(self.expect_json(404, "GET", "/v1/jobs/not-a-job")["error"], "Unknown job")
        self.assertEqual(self.expect_json(404, "DELETE", "/v1/jobs/not-a-job")["error"], "Unknown job")
        for payload in (b"{bad", b"[]", {}, {"west": "nope"}):
            bad = self.expect_json(400, "POST", "/v1/jobs", payload)
            self.assertIn("error", bad)
        oversized = self.expect_json(400, "POST", "/v1/jobs", b" " * 16385)
        self.assertEqual(oversized["error"], "Job request too large")
        self.assertEqual(self.expect_json(200, "GET", "/v1/jobs")["jobs"], [])

    def test_bounded_admission_queued_cancel_and_restart(self):
        # Hold the worker in a terrain request, so the next job is definitely
        # queued. The third submission must not create any job directory.
        self.stop_server()
        # Append to the *server* group (before [terrain]), not after it.
        content = self.config.read_text(encoding="utf-8")
        content = content.replace("max_concurrent_analyses=1\n",
                                  "max_concurrent_analyses=1\n"
                                  "max_outstanding_analyses=2\n")
        self.config.write_text(content, encoding="utf-8")
        release = threading.Event()
        self.map_server.terrain_release = release
        try:
            self.start_server()
            request = {"west": 8.0, "south": 50.0, "east": 8.004,
                       "north": 50.004, "resolution_m": 30.0,
                       "fill_depressions": False}
            first = self.expect_json(202, "POST", "/v1/jobs", request)["id"]
            second = self.expect_json(202, "POST", "/v1/jobs", request)["id"]
            self.assertEqual(self.expect_json(200, "GET", "/status")
                             ["terrain_jobs"]["outstanding"], 2)
            rejected = self.expect_json(429, "POST", "/v1/jobs", request)
            self.assertIn("capacity", rejected["error"])
            # A transient catchment spool may be present under analyses/.
            # Require *exactly* the two accepted UUID job directories: the
            # rejected submission must not leave a third job directory.
            self.assertEqual(self.persisted_job_directories(), {first, second})
            self.assertEqual(self.expect_json(202, "DELETE", f"/v1/jobs/{second}")
                             ["state"], "canceled")
            self.assertEqual(self.expect_json(200, "GET", "/status")
                             ["terrain_jobs"]["outstanding"], 1)
            replacement = self.expect_json(202, "POST", "/v1/jobs", request)["id"]
            self.assertEqual(self.expect_json(200, "GET", "/status")
                             ["terrain_jobs"]["outstanding"], 2)
            release.set()
            self.await_completed(first)
            self.await_completed(replacement)
            self.assertEqual(self.expect_json(200, "GET", f"/v1/jobs/{second}")
                             ["state"], "canceled")
            self.assertEqual(self.expect_json(200, "GET", "/status")
                             ["terrain_jobs"]["outstanding"], 0)
            self.stop_server()
            self.start_server()
            self.assertEqual(self.expect_json(200, "GET", f"/v1/jobs/{second}")
                             ["state"], "canceled")
            self.assertEqual(self.expect_json(200, "GET", "/status")
                             ["terrain_jobs"]["outstanding"], 0)
        finally:
            release.set()
            del self.map_server.terrain_release

    def test_analysis_products_rainfall_and_restart(self):
        first = self.submit()
        columns, rows = first["columns"], first["rows"]
        self.assertGreaterEqual(columns, 3)
        self.assertGreaterEqual(rows, 3)
        self.assertLess(columns, 256)
        self.assertLess(rows, 256)
        self.assertEqual(first["dataset"], "copernicus-glo30")
        self.assertEqual(first["vertical_datum"], "EGM2008")

        expected_mimes = {
            "elevation": "application/vnd.aowis.raster-f32le",
            "conditioned": "application/vnd.aowis.raster-f32le",
            "potential-fill-depth": "application/vnd.aowis.raster-f32le",
            "upstream-area": "application/vnd.aowis.raster-f64le",
            "upstream-count": "application/vnd.aowis.raster-u64le",
            "downstream": "application/vnd.aowis.raster-i64le",
        }
        snapshots = {}
        for layer, expected_mime in expected_mimes.items():
            mime, data = self.raster(first["id"], layer)
            self.assertEqual(mime, expected_mime)
            self.assertEqual(len(data), 4 * (4 if "f32" in mime else 8))
            snapshots[layer] = data
        self.assertEqual(struct.unpack("<4f", snapshots["elevation"]),
                         (1000.0, 999.0, 990.0, 989.0))
        self.assertEqual(snapshots["elevation"], snapshots["conditioned"])
        self.assertEqual(struct.unpack("<4f", snapshots["potential-fill-depth"]),
                         (0.0, 0.0, 0.0, 0.0))

        self.assertEqual(self.expect_json(400, "GET",
            f"/v1/jobs/{first['id']}/raster/elevation?x=0&y=0&width=0&height=1")["error"],
            "Invalid raster layer or window")
        self.expect_json(400, "GET", f"/v1/jobs/{first['id']}/raster/elevation?x=0")
        self.expect_json(400, "GET", f"/v1/jobs/{first['id']}/basins?limit=nonsense")
        basins = self.expect_json(200, "GET", f"/v1/jobs/{first['id']}/basins?offset=0&limit=5")
        self.assertIn("basins", basins)
        # A contributing-area threshold must be strictly positive. The
        # zero-threshold request is intentionally rejected by the service;
        # check that contract independently of the successful network query.
        invalid_threshold = self.expect_json(400, "GET",
            f"/v1/jobs/{first['id']}/network?min_upstream_area_km2=0")
        self.assertEqual(invalid_threshold["error"], "Invalid minimum contributing area")
        network = self.expect_json(200, "GET",
            f"/v1/jobs/{first['id']}/network?min_upstream_area_km2=0.000001")
        self.assertEqual(network["type"], "FeatureCollection")
        trace = self.expect_json(200, "GET", f"/v1/jobs/{first['id']}/trace?x=0&y=0")
        self.assertIn("type", trace)
        status, mime, mask = self.call("GET", f"/v1/jobs/{first['id']}/catchment?x=0&y=0")
        self.assertEqual((status, mime), (200, "application/vnd.aowis.catchment-u8"))
        self.assertEqual(len(mask), columns * rows)
        self.assertEqual(mask[0], 1)
        # Full-grid masks are also available via a bounded asynchronous path.
        # Both adapters must return bit-for-bit identical binary output.
        catchment_path = f"/v1/jobs/{first['id']}/catchment-tasks"
        self.expect_json(400, "POST", catchment_path, {"x": "0", "y": 0})
        self.expect_json(400, "POST", catchment_path, {"x": -1, "y": 0})
        submitted_catchment = self.expect_json(202, "POST", catchment_path,
                                               {"x": 0, "y": 0})
        catchment_task_path = catchment_path + "/" + submitted_catchment["task_id"]
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            catchment_state = self.expect_json(200, "GET", catchment_task_path)
            if catchment_state["state"] in ("completed", "failed", "canceled"):
                break
            time.sleep(0.025)
        self.assertEqual(catchment_state["state"], "completed", catchment_state)
        status, mime, async_mask = self.call("GET", catchment_task_path + "/mask")
        self.assertEqual((status, mime), (200, "application/vnd.aowis.catchment-u8"))
        self.assertEqual(async_mask, mask)
        self.assertEqual(self.expect_json(202, "DELETE", catchment_task_path)["state"],
                         "completed")
        self.expect_json(404, "GET", catchment_path + "/unknown")

        # A second independently computed job must give identical binary
        # products: no UUIDs, timing data or floating JSON key order involved.
        second = self.submit()
        self.assertEqual((columns, rows), (second["columns"], second["rows"]))
        for layer, expected in snapshots.items():
            self.assertEqual(self.raster(second["id"], layer)[1], expected, layer)
        self.assertEqual(len(self.expect_json(200, "GET", "/v1/jobs")["jobs"]), 2)

        rainfall = {
            "name": "HTTP contract fixture",
            "geometry": {"type": "Polygon", "coordinates": [[
                [8.0005, 50.0005], [8.0025, 50.0005],
                [8.0025, 50.0025], [8.0005, 50.0025], [8.0005, 50.0005],
            ]]},
            "depth_mm": 20, "duration_seconds": 3600,
            "effective_runoff_fraction": 0.5,
        }
        scenario = self.expect_json(201, "POST",
            f"/v1/jobs/{first['id']}/rainfall-scenarios", rainfall)
        self.assertGreater(scenario["effective_runoff_volume_m3"], 0)
        scenario_id = scenario["id"]
        scenario_path = f"/v1/jobs/{first['id']}/rainfall-scenarios/{scenario_id}"
        self.assertEqual(self.expect_json(200, "GET", scenario_path)["id"], scenario_id)
        runoff_path = scenario_path + "/runoff?x=0&y=0&width=2&height=2"
        status, mime, runoff = self.call("GET", runoff_path)
        self.assertEqual((status, mime), (200, "application/vnd.aowis.raster-f64le"))
        self.assertEqual(len(runoff), 32)
        self.assertTrue(all(value >= 0 for value in struct.unpack("<4d", runoff)))

        # The opt-in task endpoint must return without running the raster
        # calculation on the HTTP thread, and persist the same result type.
        task_path = f"/v1/jobs/{first['id']}/rainfall-scenario-tasks"
        self.expect_json(400, "POST", task_path, {"depth_mm": "bad"})
        submitted = self.expect_json(202, "POST", task_path, rainfall)
        task_id = submitted["task_id"]
        detail_path = task_path + "/" + task_id
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            async_state = self.expect_json(200, "GET", detail_path)
            if async_state["state"] in ("completed", "failed", "canceled"):
                break
            time.sleep(0.025)
        self.assertEqual(async_state["state"], "completed", async_state)
        async_scenario = async_state["scenario"]
        self.assertGreater(async_scenario["effective_runoff_volume_m3"], 0)
        async_id = async_scenario["id"]
        self.assertNotEqual(async_id, scenario_id)
        self.assertEqual(self.expect_json(200, "GET",
            f"/v1/jobs/{first['id']}/rainfall-scenarios/{async_id}")["id"], async_id)
        self.assertEqual(self.expect_json(202, "DELETE", detail_path)["state"], "completed")
        self.expect_json(404, "GET", task_path + "/unknown")

        # The next process must discover both jobs and the immutable scenario.
        # This does not yet test interrupted writes (Stage 4).
        self.stop_server()
        self.start_server()
        # Catchment tasks, unlike completed terrain analyses, are intentionally
        # transient across service restarts (and their spool is removed).
        self.expect_json(404, "GET", catchment_task_path)
        self.assertEqual(len(self.expect_json(200, "GET", "/v1/jobs")["jobs"]), 2)
        self.assertEqual(self.expect_json(200, "GET", scenario_path)["id"], scenario_id)
        for layer, expected in snapshots.items():
            self.assertEqual(self.raster(first["id"], layer)[1], expected, layer)
        self.assertEqual(self.call("GET", runoff_path)[2], runoff)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
