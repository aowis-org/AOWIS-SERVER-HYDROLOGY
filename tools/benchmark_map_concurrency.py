#!/usr/bin/env python3
"""Read-only Map DEM concurrency probe using a completed Hydrology analysis grid.

The probe repeats exactly the same Map grid patches with 1, 2, or 4
simultaneous HTTP calls. It never submits, modifies, or deletes an analysis.
This isolates the effect of request concurrency, not Map's internal CPU time.
"""

import argparse
import concurrent.futures
import hashlib
import json
import math
import sys
import time

import benchmark_map_http as single


def parse_workers(value):
    try:
        workers = [int(part) for part in value.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("--workers requires comma-separated integers") from error
    if not workers or 1 not in workers or len(workers) != len(set(workers)) or any(
            count < 1 or count > 8 for count in workers):
        raise argparse.ArgumentTypeError("--workers requires unique values from 1 to 8, including 1")
    return workers


def fetch_patch(map_url, headers, timeout, patch):
    _x, _y, columns, rows, params = patch
    data, mime, elapsed_ms = single.http_request(
        map_url, "/terrain/v1/analysis/grid", timeout,
        headers=headers, parameters=params)
    if not mime.startswith("application/vnd.aowis.dem-f32le") or len(data) != columns * rows * 4:
        raise single.BenchmarkError("Map returned an invalid analysis patch (MIME or byte length)")
    return hashlib.sha256(data).hexdigest(), len(data), elapsed_ms


def probe_round(map_url, headers, timeout, patches, workers):
    start = time.perf_counter()
    if workers == 1:
        observations = [fetch_patch(map_url, headers, timeout, patch) for patch in patches]
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            observations = list(pool.map(
                lambda patch: fetch_patch(map_url, headers, timeout, patch), patches))
    wall_ms = (time.perf_counter() - start) * 1000.0
    return {
        "wall_ms": wall_ms,
        "sum_http_ms": sum(observation[2] for observation in observations),
        "response_bytes": sum(observation[1] for observation in observations),
        "patch_sha256": [observation[0] for observation in observations],
    }


def run(args):
    if args.rounds > 20 or args.warmup_rounds > 5 or args.source_patch_size > 512:
        raise single.BenchmarkError("maximum limits: --rounds 20, --warmup-rounds 5, --source-patch-size 512")
    if not args.existing_job or len(args.existing_job) != 36 or not all(
            char in "0123456789abcdefABCDEF-" for char in args.existing_job):
        raise single.BenchmarkError("--existing-job requires a completed Hydrology analysis UUID")
    job, _ = single.http_json(args.hydrology_url,
                              "/v1/jobs/" + args.existing_job, args.request_timeout)
    if job.get("state") != "completed":
        raise single.BenchmarkError("existing Hydrology analysis must be completed")
    columns, rows = job.get("columns"), job.get("rows")
    if (not isinstance(columns, int) or isinstance(columns, bool) or
        not isinstance(rows, int) or isinstance(rows, bool) or
        columns < 1 or rows < 1 or columns * rows > args.max_cells):
        raise single.BenchmarkError("analysis cell count exceeds --max-cells or is invalid")
    if job.get("dataset") != "copernicus-glo30":
        raise single.BenchmarkError("this probe expects a Copernicus GLO-30 analysis")
    bounds = ("west", "north", "step_lon_deg", "step_lat_deg")
    if any(not isinstance(job.get(field), (float, int)) or
           not math.isfinite(job[field]) for field in bounds):
        raise single.BenchmarkError("analysis metadata lacks finite DEM grid geometry")
    if job["step_lon_deg"] <= 0 or job["step_lat_deg"] <= 0:
        raise single.BenchmarkError("DEM grid step must be positive")

    rect = {"x": 0, "y": 0, "width": columns, "height": rows}
    patches = list(single.map_acquisition_patches(job, rect, args.source_patch_size))
    if not patches or len(patches) > 256:
        raise single.BenchmarkError("analysis requires more than 256 Map grid patches")
    headers = {"X-API-Key": args.map_read_api_key} if args.map_read_api_key else {}
    outputs = {workers: [] for workers in args.workers}
    reference_hashes = None
    response_bytes = None
    for round_index in range(args.warmup_rounds + args.rounds):
        order = args.workers[round_index % len(args.workers):] + args.workers[:round_index % len(args.workers)]
        for workers in order:
            sample = probe_round(args.map_url, headers, args.request_timeout, patches, workers)
            if reference_hashes is None:
                reference_hashes = sample["patch_sha256"]
                response_bytes = sample["response_bytes"]
            if sample["patch_sha256"] != reference_hashes or sample["response_bytes"] != response_bytes:
                raise single.BenchmarkError(
                    "Map returned different DEM bytes across concurrency levels or rounds; comparison invalid")
            if round_index >= args.warmup_rounds:
                outputs[workers].append(sample)

    baseline = single.timed_summary([sample["wall_ms"] for sample in outputs[1]])["median_ms"]
    results = []
    for workers in args.workers:
        samples = outputs[workers]
        walls = single.timed_summary([sample["wall_ms"] for sample in samples])
        calls = single.timed_summary([sample["sum_http_ms"] for sample in samples])
        results.append({
            "workers": workers,
            "wall": walls,
            "sum_http": calls,
            "speedup_vs_sequential_median": round(baseline / walls["median_ms"], 3),
        })
    return {
        "schema_version": 1,
        "benchmark": "map_http_concurrency_probe",
        "map_url": args.map_url,
        "hydrology_url": args.hydrology_url,
        "existing_job": args.existing_job,
        "dataset": job["dataset"],
        "raster_columns": columns, "raster_rows": rows,
        "patch_size": args.source_patch_size,
        "patch_count": len(patches),
        "response_bytes_per_pass": response_bytes,
        "source_patches_sha256": reference_hashes,
        "rounds_per_worker": args.rounds,
        "warmup_rounds_per_worker": args.warmup_rounds,
        "results": results,
        "notes": [
            "Read-only; queries Map using completed Hydrology job grid geometry; does not submit new analyses.",
            "Map server processing, client HTTP overhead, transport and Python scheduling are all included.",
            "Each worker level fetches identical byte-verified patches; rounds rotate level order.",
            "Concurrency can increase Map CPU/memory load, so do not run against production traffic.",
            "The test does not show that Map can use parallel requests efficiently on other hardware or cold caches.",
        ],
    }


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hydrology-url", default="http://127.0.0.1:8123")
    parser.add_argument("--map-url", default="http://127.0.0.1:8122")
    parser.add_argument("--map-read-api-key", default="", help="never included in report")
    parser.add_argument("--existing-job", required=True, help="completed Hydrology job UUID")
    parser.add_argument("--source-patch-size", type=single.positive_int, default=256)
    parser.add_argument("--workers", type=parse_workers, default=[1, 2, 4])
    parser.add_argument("--rounds", type=single.positive_int, default=5)
    parser.add_argument("--warmup-rounds", type=int, default=1)
    parser.add_argument("--max-cells", type=single.positive_int, default=250000)
    parser.add_argument("--request-timeout", type=single.positive_float, default=60.0)
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.warmup_rounds < 0:
        parser.error("--warmup-rounds must be nonnegative")
    try:
        report = run(args)
    except (single.BenchmarkError, KeyError, TypeError, ValueError) as error:
        print(f"Map concurrency probe failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
