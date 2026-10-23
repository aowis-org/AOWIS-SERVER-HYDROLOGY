#!/usr/bin/env python3
"""Offline protocol fixture for the opt-in derived-cache HTTP benchmark."""

import http.server
import argparse
import importlib.util
import json
from pathlib import Path
import threading
import unittest
import uuid

MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "benchmark_derived_caches.py"
spec = importlib.util.spec_from_file_location("derived_bench", MODULE_PATH)
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)

JOB = str(uuid.UUID("11111111-1111-4111-8111-111111111111"))
SCENARIO = str(uuid.UUID("22222222-2222-4222-8222-222222222222"))
TASK = str(uuid.UUID("33333333-3333-4333-8333-333333333333"))
BASE = f"/v1/jobs/{JOB}"


class Fixture(http.server.BaseHTTPRequestHandler):
    def reply(self, code, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        root = self.server
        path = self.path.split("?", 1)[0]
        if path == "/status":
            return self.reply(200, {"status": "ok"})
        if path == BASE:
            return self.reply(200, {"state": "completed", "columns": 5, "rows": 5,
                                    "west": 8.0, "south": 50.0, "east": 8.1, "north": 50.1})
        if path == BASE + "/rainfall-scenario-tasks/" + TASK:
            return self.reply(200, {"state": "completed", "scenario": {"id": SCENARIO}})
        if path == BASE + "/rainfall-scenarios/" + SCENARIO:
            return self.reply(200, {"id": SCENARIO})
        if path == BASE + "/rainfall-scenarios/" + SCENARIO + "/redistribution/status":
            return self.reply(200, {"state": "completed" if root.built else "not_started"})
        if path == BASE + "/rainfall-scenarios/" + SCENARIO + "/redistribution":
            if root.built:
                return self.reply(200, {"cache_hit": True, "total_basins": 0,
                                        "basins": [], "outflow_m3": 12.5})
            return self.reply(409, {"error": "Redistribution result not ready"})
        return self.reply(404, {"error": "not found"})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length)) if length else {}
        if self.path == BASE + "/rainfall-scenario-tasks":
            self.server.scenario_payload = body
            return self.reply(202, {"task_id": TASK, "state": "queued"})
        if self.path == BASE + "/rainfall-scenarios/" + SCENARIO + "/redistribution":
            self.server.built = True
            return self.reply(202, {"state": "queued"})
        return self.reply(404, {"error": "not found"})

    def log_message(self, *_args):
        pass


class DerivedBenchmarkTests(unittest.TestCase):
    def setUp(self):
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
        self.server.built = False
        self.server.scenario_payload = None
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)

    def arguments(self, *args):
        values = ["--url", self.base, "--job-id", JOB, "--repeats", "3",
                  "--concurrent-clients", "4", "--poll-interval", "0.01", *args]
        return bench.parser().parse_args(values)

    def test_build_missing_and_repeated_hits(self):
        with self.assertRaisesRegex(bench.BenchmarkError, "--build-missing"):
            bench.run(self.arguments("--scenario-id", SCENARIO))
        report = bench.run(self.arguments("--scenario-id", SCENARIO, "--build-missing"))
        self.assertEqual(report["redistribution_initial_read"]["status"], 409)
        self.assertEqual(report["redistribution_poll_requests"], 1)
        self.assertEqual(len(report["redistribution_reads"]["sequential"]["samples_ms"]), 3)
        self.assertEqual(report["redistribution_reads"]["simultaneous_clients"], 4)
        self.assertRegex(report["redistribution_reads"]["sha256_canonical_json"], r"^[0-9a-f]{64}$")
        second = bench.run(self.arguments("--scenario-id", SCENARIO))
        self.assertTrue(second["redistribution_initial_read"]["cache_present"])
        self.assertNotIn("redistribution_submit_ms", second)
        self.assertEqual(report["redistribution_reads"]["sha256_canonical_json"],
                         second["redistribution_reads"]["sha256_canonical_json"])

    def test_explicit_scenario_creation(self):
        report = bench.run(self.arguments("--create-scenario", "--build-missing"))
        self.assertEqual(report["scenario_origin"], "new")
        self.assertEqual(report["scenario_id"], SCENARIO)
        points = self.server.scenario_payload["geometry"]["coordinates"][0]
        self.assertEqual(points[0], points[-1])
        self.assertEqual(self.server.scenario_payload["depth_mm"], 20)

    def test_safety_validation_and_percentiles(self):
        with self.assertRaises(argparse.ArgumentTypeError):
            bench.base_url("http://guest:pass@localhost:1234")
        with self.assertRaises(argparse.ArgumentTypeError):
            bench.uuid_arg("../../etc/passwd")
        self.assertEqual(bench.summary([7, 3, 8, 2, 4])["p95_ms"], 8)
        self.assertEqual(bench.summary([7, 3, 8, 2, 4])["median_ms"], 4)
        with self.assertRaises(SystemExit) as error:
            bench.main(["--url", self.base, "--job-id", JOB,
                        "--scenario-id", SCENARIO, "--concurrent-clients", "65"])
        self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
