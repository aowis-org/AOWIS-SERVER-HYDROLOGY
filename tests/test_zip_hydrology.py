#!/usr/bin/env python3
"""Integration checks for the Hydrology worktree/recursive-submodule handoff ZIP.

Run directly: python3 tests/test_zip_hydrology.py
Uses only disposable local Git repositories; never downloads dependencies.
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from zipfile import ZipFile


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "zip_hydrology.sh"


def git(cwd, *args):
    subprocess.run(["git", "-c", "protocol.file.allow=always", *args],
                   cwd=cwd, check=True, stdout=subprocess.PIPE,
                   stderr=subprocess.PIPE, text=True)


def init_repo(path, files):
    path.mkdir(parents=True)
    git(path, "init", "-q")
    git(path, "config", "user.name", "Test")
    git(path, "config", "user.email", "test@example.invalid")
    for filename, content in files.items():
        file = path / filename
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(content)
    git(path, "add", ".")
    git(path, "commit", "-qm", "Test fixture")
    return path


class ZipHydrologyTest(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="hydrology-zip-test-")
        self.addCleanup(self.scratch.cleanup)
        base = Path(self.scratch.name)
        nested = init_repo(base / "nested-origin", {"nested.txt": "deep"})
        module = init_repo(base / "geographic-origin", {
            "CMakeLists.txt": "# GeographicLib fixture\n",
            "include/library.h": "// available\n",
            ".gitignore": "*.o\n",
        })
        git(module, "submodule", "add", "-q", str(nested), "third_party/Nested")
        git(module, "commit", "-qam", "Include nested submodule")
        self.repo = init_repo(base / "AOWIS-SERVER-HYDROLOGY", {
            "README.md": "Hydrology fixture\n",
            ".gitignore": "*.log\n",
            "tools/benchmark_map_http.py": "#!/usr/bin/env python3\n",
        })
        benchmark = self.repo / "tools/benchmark_map_http.py"
        benchmark.chmod(0o755)
        git(self.repo, "add", "tools/benchmark_map_http.py")
        git(self.repo, "commit", "-qm", "Make benchmark executable")
        shutil.copy2(SCRIPT, self.repo / "tools/zip_hydrology.sh")
        git(self.repo, "add", "tools/zip_hydrology.sh")
        git(self.repo, "commit", "-qm", "Include pack script")
        git(self.repo, "submodule", "add", "-q", str(module), "external/GeographicLib")
        git(self.repo, "commit", "-qam", "Include GeographicLib")
        git(self.repo, "submodule", "update", "--init", "--recursive")

    def pack(self):
        destination = Path(self.scratch.name) / "packed.zip"
        result = subprocess.run(["bash", str(self.repo / "tools/zip_hydrology.sh"),
                                 str(destination)], cwd=self.repo, capture_output=True, text=True)
        return result, destination

    def test_recursive_worktrees_and_uncommitted_files(self):
        (self.repo / "new-file.cpp").write_text("// new project file")
        lib = self.repo / "external/GeographicLib"
        (lib / "new-header.h").write_text("// untracked header")
        (lib / "include/library.h").write_text("// modified tracked header")
        (lib / "ignored.o").write_text("compiled artifact")
        (self.repo / "ignored.log").write_text("not source")
        (lib / "third_party/Nested" / "untracked-deep.txt").write_text("new deep file")
        result, destination = self.pack()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("2 initialized submodules", result.stdout)
        prefix = "AOWIS-SERVER-HYDROLOGY/"
        with ZipFile(destination) as archive:
            names = set(archive.namelist())
            self.assertIn(prefix + ".gitmodules", names)
            self.assertIn(prefix + "new-file.cpp", names)
            self.assertIn(prefix + "external/GeographicLib/CMakeLists.txt", names)
            self.assertIn(prefix + "external/GeographicLib/new-header.h", names)
            self.assertIn(prefix + "external/GeographicLib/third_party/Nested/nested.txt", names)
            self.assertIn(prefix + "external/GeographicLib/third_party/Nested/untracked-deep.txt", names)
            self.assertEqual(archive.read(prefix + "external/GeographicLib/include/library.h"),
                             b"// modified tracked header")
            self.assertNotIn(prefix + "external/GeographicLib/ignored.o", names)
            self.assertNotIn(prefix + "ignored.log", names)
            self.assertFalse(any(part == ".git" for name in names for part in name.split("/")))
            self.assertTrue(bool(archive.getinfo(prefix + "tools/benchmark_map_http.py").external_attr >> 16 & 0o111))

    def test_uninitialized_submodule_fails_without_overwriting_destination(self):
        destination = Path(self.scratch.name) / "packed.zip"
        destination.write_bytes(b"original")
        git(self.repo, "submodule", "deinit", "-f", "external/GeographicLib")
        result, output = self.pack()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("git submodule update --init --recursive", result.stderr)
        self.assertEqual(output.read_bytes(), b"original")

    def test_uninitialized_nested_submodule_also_fails(self):
        git(self.repo / "external/GeographicLib", "submodule", "deinit", "-f", "third_party/Nested")
        result, destination = self.pack()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("external/GeographicLib/third_party/Nested", result.stderr)
        self.assertFalse(destination.exists())


if __name__ == "__main__":
    unittest.main()
