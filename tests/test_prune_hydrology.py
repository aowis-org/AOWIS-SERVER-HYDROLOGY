#!/usr/bin/env python3
"""Ensure offline retention tooling is conservative and opt-in."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from uuid import uuid4


TOOL = Path(__file__).resolve().parents[1] / "tools" / "prune_hydrology_analyses.py"


class PruneSafetyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="hydrology-prune-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "service" / "analyses"
        self.root.mkdir(parents=True)

    def make_job(self, state="completed", age_days=45, manifest_id=None):
        identifier = str(uuid4())
        folder = self.root / identifier
        folder.mkdir()
        manifest = folder / "job.json"
        manifest.write_text(json.dumps({"id": identifier if manifest_id is None else manifest_id,
                                        "state": state}), encoding="utf-8")
        (folder / "result.f32").write_bytes(b"valuable analysis data")
        timestamp = time.time() - age_days * 86400
        os.utime(manifest, (timestamp, timestamp))
        return folder

    def run_tool(self, *args, expected=0):
        result = subprocess.run([sys.executable, str(TOOL), "--output-directory", str(self.root),
                                 *args], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, expected, result.stderr)
        return result

    def test_dry_run_does_not_remove_results(self):
        old = self.make_job()
        report = self.run_tool("--older-than-days", "30")
        self.assertIn("DRY RUN", report.stdout)
        self.assertTrue((old / "result.f32").is_file())

    def test_only_terminal_matching_old_uuid_jobs_are_deleted(self):
        eligible = self.make_job("completed", 45)
        fresh = self.make_job("failed", 2)
        running = self.make_job("fetching-terrain", 45)
        incorrect = self.make_job("canceled", 45, manifest_id="unrelated-job")
        target = self.root / "outside"
        target.mkdir()
        (target / "important").write_text("preserve", encoding="utf-8")
        alias = self.root / str(uuid4())
        alias.symlink_to(target, target_is_directory=True)
        self.run_tool("--delete", "--confirm-server-stopped", "--older-than-days", "30")
        self.assertFalse(eligible.exists())
        for retained in (fresh, running, incorrect, target, alias):
            self.assertTrue(retained.exists() or retained.is_symlink(), retained)
        self.assertEqual((target / "important").read_text(), "preserve")

    def test_safety_guards(self):
        self.make_job()
        result = self.run_tool("--delete", expected=2)
        self.assertIn("--confirm-server-stopped", result.stderr)
        for value in ("0", "nan", "inf"):
            with self.subTest(value=value):
                self.run_tool("--older-than-days", value, expected=2)


if __name__ == "__main__":
    unittest.main()
