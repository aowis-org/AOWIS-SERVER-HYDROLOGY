#!/usr/bin/env python3
"""Explicit, offline retention cleanup of terminal Hydrology analyses.

Never run this while Hydrology is serving requests. Dry-run is the default.
Only UUID directories with matching, terminal job manifests are eligible.
Derived rainfall, redistribution, inundation and simulation artifacts inside a
selected analysis are deleted with their parent analysis.
"""

import argparse
import json
import math
from pathlib import Path
import shutil
import sys
import time
from uuid import UUID


TERMINAL = frozenset(("completed", "failed", "canceled"))


def is_job_uuid(value: str) -> bool:
    try:
        return str(UUID(value)) == value
    except ValueError:
        return False


def terminal_aged_job(folder: Path, cutoff: float) -> bool:
    if folder.is_symlink() or not folder.is_dir() or not is_job_uuid(folder.name):
        return False
    manifest = folder / "job.json"
    if manifest.is_symlink() or not manifest.is_file():
        return False
    try:
        if manifest.stat().st_mtime > cutoff:
            return False
        if manifest.stat().st_size > 1024 * 1024:
            return False
        with manifest.open("r", encoding="utf-8") as source:
            details = json.load(source)
        return (isinstance(details, dict) and details.get("id") == folder.name and
                details.get("state") in TERMINAL)
    except (OSError, UnicodeError, ValueError, TypeError):
        return False


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-directory", type=Path, required=True,
                        help="Hydrology server's analyses/ directory (server must be stopped)")
    parser.add_argument("--older-than-days", type=float, default=30.0,
                        help="Minimum age of a terminal job manifest (default: 30)")
    parser.add_argument("--delete", action="store_true",
                        help="Perform deletion instead of the default dry run")
    parser.add_argument("--confirm-server-stopped", action="store_true",
                        help="Acknowledge the Hydrology server is stopped before deleting")
    args = parser.parse_args(argv)
    if not math.isfinite(args.older_than_days) or args.older_than_days < 1:
        parser.error("--older-than-days must be a finite number >= 1")
    if args.delete and not args.confirm_server_stopped:
        parser.error("--delete requires --confirm-server-stopped")
    folder = args.output_directory
    if folder.is_symlink() or not folder.is_dir():
        parser.error("--output-directory must be an existing non-symlink directory")
    resolved = folder.resolve()
    if resolved == Path(resolved.anchor) or len(resolved.parts) < 3:
        parser.error("Refusing filesystem root or top-level directory")
    cutoff = time.time() - args.older_than_days * 86400
    eligible = 0
    deleted = 0
    for child in sorted(resolved.iterdir()):
        if not terminal_aged_job(child, cutoff):
            continue
        eligible += 1
        print(f"{'DELETE' if args.delete else 'DRY RUN'} {child}")
        if args.delete:
            # shutil.rmtree uses fd-relative, symlink-safe operations on Linux.
            # The caller must still stop the server to prevent concurrent writes.
            shutil.rmtree(child)
            deleted += 1
    print(f"Eligible: {eligible}; deleted: {deleted}; retained: other directories")
    return 0


if __name__ == "__main__":
    sys.exit(main())
