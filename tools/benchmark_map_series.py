#!/usr/bin/env python3
"""Repeated live Map -> Hydrology analysis benchmark with per-worker timing distributions.

Each measured run submits a NEW analysis job. --repeats controls only repeated
result reads per job, not the number of independent terrain acquisitions.
"""

import argparse
import json
import math
import statistics
import sys
import time

import benchmark_map_http as single


STAGE_FIELDS = (
    "terrain_fetch", "terrain_validation", "d8_analysis", "raster_persistence",
    "depression_hierarchy", "basin_persistence", "total_worker",
)
FETCH_FIELDS = (
    "map_request_ms", "raster_copy_ms", "first_request_ms",
    "fastest_request_ms", "slowest_request_ms",
)


def nonnegative_float(value):
    try:
        number = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected a finite nonnegative number") from error
    if not math.isfinite(number) or number < 0:
        raise argparse.ArgumentTypeError("expected a finite nonnegative number")
    return number


def distribution(samples):
    """Nearest-rank p90, with all observations retained for auditability."""
    if not samples:
        raise single.BenchmarkError("cannot summarize an empty benchmark series")
    if any(not isinstance(x, (int, float)) or isinstance(x, bool) or
           not math.isfinite(x) for x in samples):
        raise single.BenchmarkError("benchmark series contains an invalid measurement")
    ordered = sorted(samples)
    return {
        "count": len(samples),
        "min": round(ordered[0], 3),
        "median": round(statistics.median(ordered), 3),
        "mean": round(statistics.mean(ordered), 3),
        "p90_nearest_rank": round(ordered[math.ceil(0.9 * len(ordered)) - 1], 3),
        "max": round(ordered[-1], 3),
        "samples": [round(x, 3) for x in samples],
    }


def summarize(reports, warmup_runs=0):
    if not reports:
        raise single.BenchmarkError("at least one measured run is required")
    first = reports[0]
    reference = (first["raster_columns"], first["raster_rows"],
                 first["source_dataset"], first["basin_count"],
                 first["conditioned_cells"], first["elevation_window"]["sha256"])
    seen = set()
    ms = {name: [] for name in STAGE_FIELDS}
    ms.update({name: [] for name in FETCH_FIELDS})
    ms["fetch_unattributed_ms"] = []
    ms["job_wait_ms"] = []
    fractions = {"request_of_fetch_pct": [], "copy_of_fetch_pct": [],
                 "fetch_of_worker_pct": []}
    patch_count = None
    response_bytes = None
    transport = None
    for index, report in enumerate(reports, 1):
        identity = (report["raster_columns"], report["raster_rows"],
                    report["source_dataset"], report["basin_count"],
                    report["conditioned_cells"], report["elevation_window"]["sha256"])
        if identity != reference:
            raise single.BenchmarkError(f"run {index}: analysis output changed across identical requests")
        job_id = report["job_id"]
        if job_id in seen:
            raise single.BenchmarkError(f"run {index}: reused job ID; expected a fresh analysis")
        seen.add(job_id)
        stage = report.get("server_stage_timings_ms")
        details = report.get("server_terrain_fetch_details")
        if not isinstance(stage, dict) or not isinstance(details, dict):
            raise single.BenchmarkError(
                f"run {index}: server-side stage/fetch diagnostics missing; rebuild and restart Hydrology")
        for key in STAGE_FIELDS:
            value = stage.get(key)
            if (not isinstance(value, (int, float)) or isinstance(value, bool) or
                    not math.isfinite(value) or value < 0):
                raise single.BenchmarkError(f"run {index}: invalid stage timing {key}")
            ms[key].append(value)
        for key in FETCH_FIELDS:
            value = details.get(key)
            if (not isinstance(value, (int, float)) or isinstance(value, bool) or
                    not math.isfinite(value) or value < 0):
                raise single.BenchmarkError(f"run {index}: invalid fetch metric {key}")
            ms[key].append(value)
        count = details.get("patch_count")
        size = details.get("response_bytes")
        mode = details.get("transport")
        if (not isinstance(count, int) or isinstance(count, bool) or count < 1 or
                not isinstance(size, int) or isinstance(size, bool) or size < 1 or
                mode not in ("map_http", "map_in_process")):
            raise single.BenchmarkError(f"run {index}: invalid patch metrics")
        if patch_count is None:
            patch_count, response_bytes, transport = count, size, mode
        elif (count, size, mode) != (patch_count, response_bytes, transport):
            raise single.BenchmarkError(f"run {index}: terrain patch layout or transport changed")
        fetch = stage["terrain_fetch"]
        request = details["map_request_ms"]
        copy = details["raster_copy_ms"]
        residual = fetch - request - copy
        # Values use independently sampled monotonic clocks. Permit only tiny
        # numerical overlap, never silently label substantial overlap as zero.
        if residual < -1.0:
            raise single.BenchmarkError(f"run {index}: inconsistent terrain fetch accounting")
        ms["fetch_unattributed_ms"].append(max(0.0, residual))
        ms["job_wait_ms"].append(report["job_wait_s"] * 1000.0)
        fractions["request_of_fetch_pct"].append(request / fetch * 100.0 if fetch else 0.0)
        fractions["copy_of_fetch_pct"].append(copy / fetch * 100.0 if fetch else 0.0)
        worker = stage["total_worker"]
        fractions["fetch_of_worker_pct"].append(fetch / worker * 100.0 if worker else 0.0)
        parity = report.get("map_hydrology_parity")
        if parity is not None and parity.get("cells_outside_tolerance") != 0:
            raise single.BenchmarkError(f"run {index}: DEM parity changed")

    return {
        "schema_version": 1,
        "benchmark": "live_map_hydrology_series",
        "measured_jobs": len(reports),
        "warmup_jobs_not_reported": warmup_runs,
        "raster_columns": reference[0], "raster_rows": reference[1],
        "cells": reference[0] * reference[1],
        "dataset": reference[2], "basin_count": reference[3],
        "conditioned_cells": reference[4], "elevation_window_sha256": reference[5],
        "patch_count_per_job": patch_count,
        "map_response_bytes_per_job": response_bytes,
        "terrain_transport": transport,
        "timing_distributions_ms": {name: distribution(values) for name, values in ms.items()},
        "share_distributions_pct": {name: distribution(values) for name, values in fractions.items()},
        "jobs": reports,
        "notes": [
            "Each measured job is a separate terrain acquisition; client query repeats are within each job.",
            "Map request time includes Map processing, HTTP transport and response handling; it is NOT network-only time.",
            "Fetch unattributed time is worker fetch minus measured Map calls and raster copies, not a network latency estimate.",
            "Warmup jobs, if requested, create persistent analyses but are excluded from measurements.",
            "Server and Map caches may be warm, and concurrent external workloads can affect all measurements.",
            "Nearest-rank p90 is descriptive only; five observations cannot establish a stable tail latency.",
        ],
    }


def run_series(args, on_progress=None):
    if args.existing_job:
        raise single.BenchmarkError("multi-job benchmark requires new jobs; --existing-job is not supported")
    if args.runs < 2 or args.runs > 20 or args.warmup_runs < 0 or args.warmup_runs > 5:
        raise single.BenchmarkError("--runs must be 2..20 and --warmup-runs 0..5")
    if args.inter_run_seconds > 30 or args.window > 512 or args.source_patch_size > 512 or args.basin_limit > 100 or args.repeats > 100:
        raise single.BenchmarkError("invalid HTTP benchmark limits")
    if args.poll_seconds > 60:
        raise single.BenchmarkError("--poll-seconds must be <=60")
    if None in (args.west, args.south, args.east, args.north):
        raise single.BenchmarkError("--west --south --east --north are required")
    dimensions = single.estimated_grid((args.west, args.south, args.east, args.north), args.resolution_m)
    if dimensions[0] * dimensions[1] > args.max_cells:
        raise single.BenchmarkError("estimated cells exceed --max-cells; no jobs were submitted")
    reports = []
    total = args.warmup_runs + args.runs
    for index in range(total):
        if on_progress:
            on_progress(index + 1, total, index < args.warmup_runs)
        try:
            report = single.run(args)
        except (single.BenchmarkError, KeyError, TypeError, ValueError) as error:
            raise single.BenchmarkError(f"job {index + 1}/{total} failed: {error}") from error
        if index >= args.warmup_runs:
            reports.append(report)
        if index + 1 < total and args.inter_run_seconds:
            time.sleep(args.inter_run_seconds)
    return summarize(reports, args.warmup_runs)


def build_parser():
    parser = single.build_parser()
    parser.description = __doc__
    parser.set_defaults(repeats=1)
    parser.add_argument("--runs", type=single.positive_int, default=5,
                        help="number of independent, newly submitted measured analyses (2..20)")
    parser.add_argument("--warmup-runs", type=int, default=0,
                        help="optional new analysis jobs before measurement (0..5; also persisted)")
    parser.add_argument("--inter-run-seconds", type=nonnegative_float, default=0.0,
                        help="time between independent jobs (0..30)")
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        report = run_series(args, lambda i, total, warmup:
                            print(f"{'warmup' if warmup else 'measured'} job {i}/{total}", file=sys.stderr))
    except (single.BenchmarkError, KeyError, TypeError, ValueError) as error:
        print(f"Benchmark series failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
