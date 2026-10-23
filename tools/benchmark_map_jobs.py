#!/usr/bin/env python3
"""Measure bounded simultaneous Hydrology analysis jobs against the live Map server.

Standard library only. Each wave creates independent persistent jobs; measurements
stop at job completion BEFORE any raster/Map verification requests are issued.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import math
import os
import pathlib
import statistics
import sys
import threading
import time
import urllib.parse

import benchmark_map_http as single
from benchmark_map_series import distribution


STAGES = ("terrain_fetch", "d8_analysis", "depression_hierarchy", "total_worker")


def levels_arg(value):
    try:
        values = [int(part) for part in value.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("--jobs requires comma-separated integers") from error
    if not values or len(values) > 4 or len(set(values)) != len(values) or 1 not in values or any(v < 1 or v > 4 for v in values):
        raise argparse.ArgumentTypeError("--jobs must include 1 and have unique levels from 1 to 4")
    return values


def nonnegative_int(value):
    try:
        number = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected a nonnegative integer") from error
    if number < 0:
        raise argparse.ArgumentTypeError("expected a nonnegative integer")
    return number


def _positive_pid(value):
    return single.positive_int(value)


def nonnegative_float(value):
    try:
        number = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected a nonnegative number") from error
    if not math.isfinite(number) or number < 0:
        raise argparse.ArgumentTypeError("expected a nonnegative finite number")
    return number


def build_parser():
    parser = single.build_parser()
    parser.description = __doc__
    parser.set_defaults(map_url="", repeats=1)
    parser.add_argument("--jobs", type=levels_arg, default=[1, 2, 4],
                        help="concurrent analysis jobs per wave; comma-separated unique 1..4, including 1")
    parser.add_argument("--rounds", type=single.positive_int, default=3,
                        help="measured waves per concurrency level (1..5)")
    parser.add_argument("--warmup-rounds", type=nonnegative_int, default=0,
                        help="unmeasured warmup waves per level (0..2); also persist jobs")
    parser.add_argument("--max-batch-cells", type=single.positive_int, default=1000000,
                        help="preflight safety budget for simultaneous grid cells")
    parser.add_argument("--max-total-cells", type=single.positive_int, default=8000000,
                        help="preflight safety budget across all newly persisted jobs")
    parser.add_argument("--map-pid", type=_positive_pid,
                        help="optional local Linux Map server PID for /proc VmRSS samples")
    parser.add_argument("--hydrology-pid", type=_positive_pid,
                        help="optional local Linux Hydrology PID for /proc VmRSS samples")
    parser.add_argument("--rss-poll-seconds", type=single.positive_float, default=0.025,
                        help="optional /proc sampling interval during waves (0.01..1 s)")
    parser.add_argument("--rss-settle-seconds", type=nonnegative_float, default=2.0,
                        help="post-wave delay before reading RSS again, with PIDs only (0..10 s; default 2)")
    return parser


def validate_args(args):
    if args.existing_job:
        raise single.BenchmarkError("only NEW jobs may be submitted; --existing-job is unsupported")
    if None in (args.west, args.south, args.east, args.north):
        raise single.BenchmarkError("--west --south --east --north are required")
    if args.rounds > 5 or args.warmup_rounds > 2:
        raise single.BenchmarkError("--rounds must be 1..5 and --warmup-rounds 0..2")
    if (args.window > 512 or args.source_patch_size > 512 or args.poll_seconds > 60 or
            args.basin_limit > 100 or args.repeats != 1):
        raise single.BenchmarkError("invalid window, patch size, polling interval, basin limit or repeats")
    if not 0.01 <= args.rss_poll_seconds <= 1:
        raise single.BenchmarkError("--rss-poll-seconds must be 0.01..1")
    if not 0 <= args.rss_settle_seconds <= 10:
        raise single.BenchmarkError("--rss-settle-seconds must be 0..10")
    columns, rows = single.estimated_grid((args.west, args.south, args.east, args.north), args.resolution_m)
    cells = columns * rows
    if cells > args.max_cells:
        raise single.BenchmarkError("estimated cells per job exceed --max-cells; no jobs submitted")
    if cells * max(args.jobs) > args.max_batch_cells:
        raise single.BenchmarkError("simultaneous cells exceed --max-batch-cells; no jobs submitted")
    total_jobs = sum(args.jobs) * (args.rounds + args.warmup_rounds)
    if total_jobs > 40 or cells * total_jobs > args.max_total_cells:
        raise single.BenchmarkError("total job count or cells exceed safety budget; no jobs submitted")
    if args.map_pid and args.hydrology_pid and args.map_pid == args.hydrology_pid:
        raise single.BenchmarkError("Map and Hydrology PIDs must be different")
    for pid in (args.map_pid, args.hydrology_pid):
        if pid is not None:
            read_rss_kib(pid)  # preflight Linux /proc before creating jobs
    return columns, rows, cells, total_jobs


def read_rss_kib(pid):
    path = pathlib.Path("/proc") / str(pid) / "status"
    try:
        content = path.read_text(encoding="utf-8")
        for line in content.splitlines():
            if line.startswith("VmRSS:"):
                parts = line.split()
                if len(parts) == 3 and parts[2] == "kB":
                    return int(parts[1])
    except (OSError, ValueError) as error:
        raise single.BenchmarkError(f"cannot sample Linux RSS for PID {pid}: {error}") from error
    raise single.BenchmarkError(f"cannot sample Linux RSS for PID {pid}: missing VmRSS")


class RssSampler:
    def __init__(self, pids, interval):
        self.pids = pids
        self.interval = interval
        self.data = {name: [] for name in pids}
        self.stop_event = threading.Event()
        self.thread = None
        self.error = None

    def _loop(self):
        while not self.stop_event.is_set():
            try:
                for name, pid in self.pids.items():
                    self.data[name].append(read_rss_kib(pid))
            except single.BenchmarkError as error:
                self.error = error
                break
            self.stop_event.wait(self.interval)

    def __enter__(self):
        if self.pids:
            self.thread = threading.Thread(target=self._loop, name="hydrology-benchmark-rss", daemon=True)
            self.thread.start()
        return self

    def __exit__(self, exc_type, exc, tb):
        self.stop_event.set()
        if self.thread is not None:
            self.thread.join()
        if self.error and exc_type is None:
            raise self.error

    def summary(self):
        if not self.pids:
            return None
        return {name: {"samples": len(values), "initial_kib": values[0],
                       "peak_kib": max(values), "last_kib": values[-1],
                       "peak_increase_kib": max(values) - values[0]}
                for name, values in self.data.items() if values}


def submit_and_poll(args, barrier, slot):
    """Complete one job; do NOT query Map or read completed rasters yet."""
    try:
        barrier.wait(timeout=15.0)
    except threading.BrokenBarrierError as error:
        raise single.BenchmarkError("concurrent submission barrier failed") from error
    started = time.perf_counter()
    body = {"west": args.west, "south": args.south, "east": args.east, "north": args.north,
            "resolution_m": args.resolution_m, "dataset": args.dataset,
            "fill_depressions": args.fill_depressions}
    metadata, submit_ms = single.http_json(args.hydrology_url, "/v1/jobs", args.request_timeout,
                                           method="POST", body=body)
    job_id = metadata.get("id")
    if not isinstance(job_id, str) or not job_id:
        raise single.BenchmarkError("Hydrology submission returned no job ID")
    polls = 0
    while metadata.get("state") not in ("completed", "failed", "canceled"):
        if time.perf_counter() - started >= args.job_timeout:
            raise single.BenchmarkError(f"job {job_id} timed out (server job remains active)")
        time.sleep(args.poll_seconds)
        metadata, _ = single.http_json(args.hydrology_url, "/v1/jobs/" +
                                       urllib.parse.quote(job_id, safe=""), args.request_timeout)
        polls += 1
    if metadata.get("id") != job_id:
        raise single.BenchmarkError(f"job {job_id} metadata identity changed")
    if metadata["state"] != "completed":
        raise single.BenchmarkError(f"job {job_id} ended {metadata['state']}: {metadata.get('error', '')}")
    finish = time.perf_counter()
    stage = metadata.get("stage_timings_ms")
    fetch = metadata.get("terrain_fetch_details")
    if not isinstance(stage, dict) or not isinstance(fetch, dict):
        raise single.BenchmarkError(f"job {job_id}: missing worker performance diagnostics")
    for field in STAGES:
        value = stage.get(field)
        if not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(value) or value < 0:
            raise single.BenchmarkError(f"job {job_id}: invalid timing {field}")
    queue_wait = stage.get("queue_wait")
    if queue_wait is not None and (not isinstance(queue_wait, (int, float)) or
            isinstance(queue_wait, bool) or not math.isfinite(queue_wait) or queue_wait < 0):
        raise single.BenchmarkError(f"job {job_id}: invalid timing queue_wait")
    peak = fetch.get("peak_in_flight")
    if not isinstance(peak, int) or isinstance(peak, bool) or not 1 <= peak <= 4:
        raise single.BenchmarkError(f"job {job_id}: invalid Map peak_in_flight")
    cols, rows = metadata.get("columns"), metadata.get("rows")
    if not isinstance(cols, int) or isinstance(cols, bool) or not isinstance(rows, int) or isinstance(rows, bool) or cols < 1 or rows < 1 or cols * rows > args.max_cells:
        raise single.BenchmarkError(f"job {job_id}: invalid result dimensions")
    if metadata.get("dataset") != args.dataset:
        raise single.BenchmarkError(f"job {job_id}: unexpected dataset")
    for field in ("basin_count", "conditioned_cells"):
        value = metadata.get(field)
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise single.BenchmarkError(f"job {job_id}: invalid {field}")
    if any(stage[field] > stage["total_worker"] + 1.0 for field in STAGES):
        raise single.BenchmarkError(f"job {job_id}: stage duration exceeds worker duration")
    return {"slot": slot, "job_id": job_id, "submitted_at": started,
            "completed_at": finish, "latency_ms": (finish - started) * 1000,
            "submit_ms": submit_ms, "polls": polls,
            "columns": cols, "rows": rows, "dataset": metadata.get("dataset"),
            "basin_count": metadata.get("basin_count"),
            "conditioned_cells": metadata.get("conditioned_cells"),
            "stage_timings_ms": {field: stage[field] for field in STAGES},
            "queue_wait_ms": queue_wait,
            "terrain_peak_in_flight": peak,
            "metadata": metadata}


def verify_wave(args, jobs):
    """Verify after timing ends; avoid parity traffic competing with active jobs."""
    map_headers = {"X-API-Key": args.map_read_api_key} if args.map_read_api_key else {}
    reference = None
    for job in jobs:
        columns, rows = job["columns"], job["rows"]
        rect = {"x": max(0, (columns - args.window) // 2),
                "y": max(0, (rows - args.window) // 2),
                "width": min(columns, args.window), "height": min(rows, args.window)}
        data, mime, _ = single.http_request(args.hydrology_url,
            "/v1/jobs/" + urllib.parse.quote(job["job_id"], safe="") + "/raster/elevation",
            args.request_timeout, parameters=rect)
        if mime != "application/vnd.aowis.raster-f32le" or len(data) != rect["width"] * rect["height"] * 4:
            raise single.BenchmarkError(f"job {job['job_id']}: invalid persisted elevation window")
        digest = hashlib.sha256(data).hexdigest()
        identity = (job["columns"], job["rows"], job["dataset"],
                    job["basin_count"], job["conditioned_cells"], digest)
        if reference is None:
            reference = identity
            if args.map_url:
                map_data, _, _, _ = single.fetch_original_map_window(args.map_url, args.request_timeout,
                    map_headers, job["metadata"], rect, args.source_patch_size)
                if map_data != data:
                    raise single.BenchmarkError("Map/Hydrology source DEM parity failed")
        elif identity != reference:
            raise single.BenchmarkError(f"job {job['job_id']}: nonidentical analysis output")
        job["elevation_window_sha256"] = digest
        job.pop("metadata")
    return reference


def run_wave(args, level, round_number, warmup=False):
    barrier = threading.Barrier(level)
    pids = {name: pid for name, pid in (("map", args.map_pid), ("hydrology", args.hydrology_pid)) if pid is not None}
    wave_start = time.perf_counter()
    with RssSampler(pids, args.rss_poll_seconds) as sampler:
        with ThreadPoolExecutor(max_workers=level, thread_name_prefix="hydrology-load") as pool:
            futures = [pool.submit(submit_and_poll, args, barrier, slot) for slot in range(level)]
            jobs = []
            errors = []
            for future in as_completed(futures):
                try:
                    jobs.append(future.result())
                except (single.BenchmarkError, KeyError, TypeError, ValueError) as error:
                    errors.append(error)
            # Stop the timed interval at completion, BEFORE worker cleanup,
            # RSS settling or persisted raster verification.
            wave_end = time.perf_counter()
    if errors:
        raise single.BenchmarkError(f"{level}-job wave {round_number}: {errors[0]}; other submitted jobs may persist")
    jobs.sort(key=lambda job: job["slot"])
    if len({job["job_id"] for job in jobs}) != level:
        raise single.BenchmarkError("Hydrology reused a job ID during simultaneous submissions")
    rss = sampler.summary()
    if rss is not None:
        # A completed job can still own DEM/D8/hierarchy arrays briefly.
        # Observe a separate post-wave point, WITHOUT adding the delay to
        # throughput or letting raster verification alter the measurement.
        if args.rss_settle_seconds > 0:
            time.sleep(args.rss_settle_seconds)
        for name, pid in pids.items():
            if name not in rss:
                raise single.BenchmarkError(f"missing wave RSS samples for {name}")
            settled = read_rss_kib(pid)
            rss[name]["settled_kib"] = settled
            rss[name]["settled_change_kib"] = settled - rss[name]["initial_kib"]
    reference = verify_wave(args, jobs)
    return {"jobs": level, "round": round_number, "warmup": warmup,
            "completion_wall_ms": (wave_end - wave_start) * 1000,
            "completed_jobs_per_second": level / (wave_end - wave_start),
            "rss": rss, "jobs_details": [{k: v for k, v in job.items()
                if k not in ("submitted_at", "completed_at")} for job in jobs],
            "identity": reference}


def summarize(waves, levels, rounds, warmup_rounds, expected_cells):
    if not waves:
        raise single.BenchmarkError("cannot summarize empty job concurrency benchmark")
    baseline = None
    results = []
    reference = waves[0]["identity"]
    for level in levels:
        batch = [wave for wave in waves if wave["jobs"] == level]
        if len(batch) != rounds:
            raise single.BenchmarkError("incomplete concurrency measurements")
        if any(wave["identity"] != reference for wave in batch):
            raise single.BenchmarkError("analysis output differs across concurrency levels")
        job_latencies = [job["latency_ms"] for wave in batch for job in wave["jobs_details"]]
        stage_dists = {field: distribution([job["stage_timings_ms"][field]
            for wave in batch for job in wave["jobs_details"]]) for field in STAGES}
        queue_waits = [job.get("queue_wait_ms") for wave in batch
                       for job in wave["jobs_details"]]
        if any(value is None for value in queue_waits) and not all(
                value is None for value in queue_waits):
            raise single.BenchmarkError("mixed queue-wait diagnostics across jobs")
        rss_samples = [wave.get("rss") for wave in batch]
        if all(sample is None for sample in rss_samples):
            rss_stats = None
        elif any(sample is None for sample in rss_samples):
            raise single.BenchmarkError("mixed RSS sampling across measured waves")
        else:
            process_names = set(rss_samples[0])
            if any(set(sample) != process_names for sample in rss_samples):
                raise single.BenchmarkError("inconsistent RSS processes across measured waves")
            rss_stats = {name: {field: distribution([sample[name][field] for sample in rss_samples])
                for field in ("peak_kib", "peak_increase_kib", "settled_kib", "settled_change_kib")}
                for name in sorted(process_names)}
        group = {"jobs": level, "waves": rounds, "rss_kib": rss_stats,
                 "queue_wait_ms": (distribution(queue_waits)
                                   if all(value is not None for value in queue_waits) else None),
                 "wall_ms": distribution([wave["completion_wall_ms"] for wave in batch]),
                 "throughput_jobs_per_second": distribution([wave["completed_jobs_per_second"] for wave in batch]),
                 "job_latency_ms": distribution(job_latencies),
                 "stage_timings_ms": stage_dists,
                 "per_job_map_peak_in_flight": distribution([job["terrain_peak_in_flight"] for wave in batch for job in wave["jobs_details"]])}
        if level == 1:
            baseline = group["wall_ms"]["median"]
        results.append(group)
    for result in results:
        result["relative_throughput_vs_single"] = round(
            result["jobs"] * baseline / result["wall_ms"]["median"], 3)
    return {"schema_version": 1, "benchmark": "live_map_hydrology_concurrent_jobs",
            "concurrency_levels": levels, "measured_rounds_per_level": rounds,
            "warmup_rounds_per_level": warmup_rounds, "total_jobs_created":
                sum(levels) * (rounds + warmup_rounds),
            "expected_cells_per_job": expected_cells,
            "raster_columns": reference[0], "raster_rows": reference[1],
            "dataset": reference[2], "basin_count": reference[3],
            "conditioned_cells": reference[4], "elevation_window_sha256": reference[5],
            "results": results, "measured_waves": [{k: v for k, v in wave.items()
                if k != "identity"} for wave in waves],
            "notes": [
                "All waves create new persisted Hydrology analyses; this tool never deletes jobs.",
                "Completion wall time ends when the last job completes, before checking any saved elevation rasters or Map parity.",
                "Measured throughput is per batch, not sustainable throughput or server admission capacity.",
                "Each job may simultaneously issue up to four Map HTTP patch requests; the jobs limit bounds concurrent clients, not global Map requests.",
                "Linux RSS samples are optional and measure the supplied processes' resident memory, not peak aggregate system memory.",
                "settled_kib is read after --rss-settle-seconds of post-wave delay, before raster verification; it is not evidence of complete reclamation or absence of leaks.",
                "Post-wave memory delay and verification are excluded from all completion wall times and throughput.",
                "Map parity is checked only after each wave; Map calls are not part of the completion wall time.",
                "Round order is rotated; changing caches, CPU load, memory and disk I/O can affect every measurement.",
                "Queue wait is measured by Hydrology before worker execution and does not count toward total_worker.",
                "Absent queue-wait metadata from older servers is reported as null.",
            ]}


def benchmark(args, on_progress=None):
    columns, rows, cells, total_jobs = validate_args(args)
    status, _ = single.http_json(args.hydrology_url, "/status", args.request_timeout)
    if status.get("status") != "ok":
        raise single.BenchmarkError("Hydrology server status is not ok")
    waves = []
    reference = None
    total_waves = len(args.jobs) * (args.rounds + args.warmup_rounds)
    wave_index = 0
    for iteration in range(args.warmup_rounds + args.rounds):
        for level in args.jobs[iteration % len(args.jobs):] + args.jobs[:iteration % len(args.jobs)]:
            wave_index += 1
            if on_progress:
                on_progress(wave_index, total_waves, level, iteration < args.warmup_rounds)
            wave = run_wave(args, level, iteration + 1, iteration < args.warmup_rounds)
            if reference is None:
                reference = wave["identity"]
            elif wave["identity"] != reference:
                raise single.BenchmarkError("analysis outputs changed across waves")
            if iteration >= args.warmup_rounds:
                waves.append(wave)
    return summarize(waves, args.jobs, args.rounds, args.warmup_rounds, cells)


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        report = benchmark(args, lambda i, total, count, warmup: print(
            f"{'warmup' if warmup else 'measured'} wave {i}/{total}: {count} jobs", file=sys.stderr))
    except (single.BenchmarkError, KeyError, ValueError, TypeError) as error:
        print(f"Concurrent Hydrology benchmark failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
