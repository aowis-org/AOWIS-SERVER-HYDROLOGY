#!/usr/bin/env python3
"""Regression coverage for Hydrology's INI generation and discovery CLI."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class ServerConfigurationCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if len(sys.argv) != 2:
            raise RuntimeError("expected path to aowis-server-hydrology")
        cls.executable = Path(sys.argv[1]).resolve()
        sys.argv[1:] = []

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="aowis-hydrology-config-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.environment = os.environ.copy()
        self.environment["XDG_DATA_HOME"] = str(self.root / "xdg-data")

    def run_cli(self, *args, success=True):
        result = subprocess.run(
            [str(self.executable), *map(str, args)],
            env=self.environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=15,
            check=False,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def test_explicit_path_and_overwrite_safeguard(self):
        target = self.root / "custom" / "nested" / "server.ini"
        self.assertIn(str(target), self.run_cli("--write-default-config", target).stdout)
        self.assertIn("max_concurrent_analyses=1", target.read_text(encoding="utf-8"))
        self.assertIn("max_outstanding_analyses=8", target.read_text(encoding="utf-8"))
        self.assertIn("max_coupled_dem_cells=10000000", target.read_text(encoding="utf-8"))
        self.assertIn("max_active_terrain_cells=0", target.read_text(encoding="utf-8"))
        content = target.read_bytes()
        self.run_cli("--write-default-config", target, success=False)
        self.assertEqual(target.read_bytes(), content)
        target.write_text("[server]\nmax_concurrent_analyses=2\n", encoding="utf-8")
        self.run_cli("--write-default-config", target, "--overwrite")
        self.assertEqual(target.read_bytes(), content)
        self.run_cli("--config", target, "--check-config")

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux XDG path regression")
    def test_default_path_generation_and_automatic_loading(self):
        target = self.root / "xdg-data" / "aowis-server-hydrology" / "hydrology.ini"
        self.assertFalse(target.exists())
        self.assertIn(str(target), self.run_cli("--write-default-config").stdout)
        self.assertTrue(target.is_file())
        self.assertIn("dataset=copernicus-glo30", target.read_text(encoding="utf-8"))
        self.assertIn(str(target), self.run_cli("--check-config").stdout)

        target.write_text("[server]\nmax_concurrent_analyses=100\n", encoding="utf-8")
        self.assertIn("max_concurrent_analyses", self.run_cli("--check-config", success=False).stderr)
        self.run_cli("--write-default-config", success=False)
        self.run_cli("--write-default-config", "--overwrite")
        self.run_cli("--check-config")

    def test_printed_sample_is_a_parseable_ini(self):
        example = self.run_cli("--print-default-config").stdout
        self.assertIn("; AOWIS Hydrology service", example)
        self.assertIn("; Independent analysis workers", example)
        self.assertFalse(any(line.lstrip().startswith("#") for line in example.splitlines()))
        target = self.root / "from-print-default.ini"
        target.write_text(example, encoding="utf-8")
        self.run_cli("--config", target, "--check-config")

    def test_terrain_job_capacity_config_validation(self):
        target = self.root / "capacity.ini"
        invalid = (
            ("max_outstanding_analyses=0", "max_outstanding_analyses"),
            ("max_outstanding_analyses=65", "max_outstanding_analyses"),
            ("max_outstanding_analyses=abc", "max_outstanding_analyses"),
            ("max_concurrent_analyses=3\nmax_outstanding_analyses=2",
             "max_outstanding_analyses"),
        )
        for setting, expected_error in invalid:
            with self.subTest(setting=setting):
                target.write_text("[server]\n" + setting + "\n", encoding="utf-8")
                self.assertIn(expected_error,
                              self.run_cli("--config", target, "--check-config",
                                           success=False).stderr)
        target.write_text("[server]\nmax_concurrent_analyses=2\n"
                          "max_outstanding_analyses=2\n", encoding="utf-8")
        self.run_cli("--config", target, "--check-config")

    def test_active_terrain_cell_budget_validation(self):
        target = self.root / "active-cells.ini"
        for setting in ("max_active_terrain_cells=-1", "max_active_terrain_cells=3",
                        "max_active_terrain_cells=400000001", "max_active_terrain_cells=abc"):
            with self.subTest(setting=setting):
                target.write_text("[server]\n" + setting + "\n", encoding="utf-8")
                self.assertIn("max_active_terrain_cells", self.run_cli(
                    "--config", target, "--check-config", success=False).stderr)
        for amount in (0, 4, 10000000, 400000000):
            target.write_text(f"[server]\nmax_active_terrain_cells={amount}\n",
                              encoding="utf-8")
            self.run_cli("--config", target, "--check-config")

    def test_coupled_dem_budget_validation(self):
        target = self.root / "coupled-limits.ini"
        for setting in ("max_coupled_dem_cells=0", "max_coupled_dem_cells=100000001",
                        "max_coupled_dem_cells=not-a-number"):
            target.write_text("[server]\n" + setting + "\n", encoding="utf-8")
            self.assertIn("max_coupled_dem_cells", self.run_cli(
                "--config", target, "--check-config", success=False).stderr)
        target.write_text("[server]\nmax_cells=100000000\n"
                          "max_coupled_dem_cells=100000000\n", encoding="utf-8")
        self.run_cli("--config", target, "--check-config")

    def test_http_transport_limits(self):
        target = self.root / "http-limits.ini"
        for setting, key in (
            ("http_workers=0", "http_workers"),
            ("http_workers=65", "http_workers"),
            ("http_max_queued_requests=0", "http_max_queued_requests"),
            ("http_max_queued_requests=1025", "http_max_queued_requests"),
            ("http_read_timeout_s=0", "http_read_timeout_s"),
            ("http_write_timeout_s=601", "http_write_timeout_s"),
            ("http_keep_alive_timeout_s=121", "http_keep_alive_timeout_s"),
        ):
            with self.subTest(setting=setting):
                target.write_text("[server]\n" + setting + "\n", encoding="utf-8")
                self.assertIn(key, self.run_cli("--config", target,
                                               "--check-config", success=False).stderr)
        target.write_text("[server]\nhttp_workers=4\n"
                          "http_max_queued_requests=8\n"
                          "http_read_timeout_s=10\n"
                          "http_write_timeout_s=30\n"
                          "http_keep_alive_timeout_s=3\n", encoding="utf-8")
        self.run_cli("--config", target, "--check-config")

    def test_equals_form_is_compatible(self):
        target = self.root / "equals" / "custom.ini"
        self.run_cli(f"--write-default-config={target}")
        self.assertTrue(target.is_file())

    def test_reject_multiple_positional_paths(self):
        self.assertIn("at most one", self.run_cli(
            "--write-default-config", self.root / "a.ini", self.root / "b.ini", success=False
        ).stderr)

    def test_reject_unexpected_positional_path(self):
        self.assertIn("Unexpected positional", self.run_cli(
            "--check-config", self.root / "other.ini", success=False
        ).stderr)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
