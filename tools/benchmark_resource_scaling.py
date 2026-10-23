#!/usr/bin/env python3
"""Measure independent core benchmark processes at increasing grid sizes.

This reports observed whole-process RSS, not a safe upper bound or an estimate
of the server's combined memory use. One executable is launched per sample.
"""
import argparse
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys


def size_arg(value: str) -> tuple[int, int]:
    try:
        columns, rows = (int(v) for v in value.lower().split("x"))
    except (ValueError, TypeError):
        raise argparse.ArgumentTypeError("grid size must be COLUMNSxROWS") from None
    if not (16 <= columns <= 5000 and 16 <= rows <= 5000 and columns * rows <= 10_000_000):
        raise argparse.ArgumentTypeError("grid size must satisfy 16..5000 on each axis, <=10M cells")
    return columns, rows


def gather(executable: Path, size: tuple[int, int], *, terrain: str, seed: int,
           timeout: int) -> dict:
    columns, rows = size
    command = [str(executable), "--columns", str(columns), "--rows", str(rows),
               "--terrain", terrain, "--seed", str(seed)]
    process = subprocess.run(command, capture_output=True, text=True,
                             check=False, timeout=timeout)
    if process.returncode != 0:
        raise RuntimeError(f"benchmark returned {process.returncode}: {process.stderr[-2000:]}")
    try:
        data = json.loads(process.stdout)
    except (json.JSONDecodeError, ValueError) as exc:
        raise RuntimeError("benchmark returned malformed JSON") from exc
    if (data.get("cells") != columns * rows or data.get("terrain") != terrain or
            data.get("seed") != seed or data.get("benchmark") != "synthetic_hydrology_core"):
        raise RuntimeError("benchmark reported inconsistent input identity")
    for name in ("peak_process_rss_bytes", "measured_stages_total_ms"):
        value = data.get(name)
        if value is not None and (not isinstance(value, (int, float)) or
                                  isinstance(value, bool) or not math.isfinite(value) or value < 0):
            raise RuntimeError(f"invalid {name} in benchmark output")
    return data


def summarize(samples: list[dict], size: tuple[int, int]) -> dict:
    count = size[0] * size[1]
    times = [sample["measured_stages_total_ms"] for sample in samples]
    rss = [sample.get("peak_process_rss_bytes") for sample in samples]
    known = [value for value in rss if value is not None]
    return {
        "columns": size[0], "rows": size[1], "cells": count,
        "samples": len(samples),
        "stage_time_ms": {"min": min(times), "median": statistics.median(times),
                          "max": max(times)},
        "peak_process_rss_bytes": (
            {"min": min(known), "median": statistics.median(known),
             "max": max(known)} if known else None),
        "observed_peak_rss_bytes_per_cell": (statistics.median(known) / count
                                             if known else None),
        "identities": sorted(set(sample["elevation_fingerprint_fnv64"] for sample in samples)),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--size", type=size_arg, action="append", dest="sizes",
                        help="COLUMNSxROWS (repeat for multiple sizes)")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--terrain", choices=("rolling", "pitted", "flat"), default="pitted")
    parser.add_argument("--seed", type=int, default=17)
    parser.add_argument("--timeout-seconds", type=int, default=300)
    args = parser.parse_args(argv)
    if not (1 <= args.repeats <= 20) or not (0 <= args.seed <= 4294967295) or args.timeout_seconds < 1:
        parser.error("repeats must be 1..20, seed 0..4294967295, timeout >= 1")
    sizes = args.sizes or [(256, 256), (512, 512), (1024, 1024)]
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error(f"benchmark executable not found: {executable}")
    results = []
    try:
        for size in sizes:
            samples = [gather(executable, size, terrain=args.terrain, seed=args.seed,
                              timeout=args.timeout_seconds) for _ in range(args.repeats)]
            results.append(summarize(samples, size))
    except (OSError, subprocess.TimeoutExpired, RuntimeError) as exc:
        print(f"Resource benchmark failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps({
        "schema": "aowis_hydrology_resource_scaling_v1",
        "scope": "sequential independent core benchmark processes",
        "terrain": args.terrain, "seed": args.seed,
        "peak_rss_scope": "entire Linux benchmark process; unknown on other systems",
        "memory_bound": False,
        "not_measured": ["Qt service", "Map", "HTTP", "concurrent workload", "disk cache"],
        "results": results,
    }, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
