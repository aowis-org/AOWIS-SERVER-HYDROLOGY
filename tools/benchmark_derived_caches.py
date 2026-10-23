#!/usr/bin/env python3
"""Opt-in HTTP benchmark of persistent Hydrology derived-cache creation and reads.

Uses an existing completed terrain analysis. Does not delete caches, stop servers,
modify source terrain, or implicitly run network operations against public hosts.
Python standard library only.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
import statistics
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid


class BenchmarkError(RuntimeError):
    pass


def positive_int(value):
    try:
        number = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected a positive integer") from exc
    if number < 1:
        raise argparse.ArgumentTypeError("expected a positive integer")
    return number


def base_url(value):
    url = urllib.parse.urlsplit(value)
    if url.scheme not in ("http", "https") or not url.hostname or url.username or url.password or url.query or url.fragment:
        raise argparse.ArgumentTypeError("expected an HTTP(S) URL with no credentials, query or fragment")
    return value.rstrip("/")


def uuid_arg(value):
    try:
        canonical = str(uuid.UUID(value))
    except (ValueError, AttributeError) as exc:
        raise argparse.ArgumentTypeError("expected a canonical UUID") from exc
    if canonical != value:
        raise argparse.ArgumentTypeError("expected a lowercase canonical UUID")
    return value


def summary(samples):
    if not samples:
        raise BenchmarkError("at least one timing sample is required")
    ordered = sorted(samples)
    # Nearest-rank p95, suitable even for a small number of samples.
    p95 = ordered[math.ceil(.95 * len(ordered)) - 1]
    return {
        "samples_ms": [round(x, 3) for x in samples],
        "median_ms": round(statistics.median(samples), 3),
        "p95_ms": round(p95, 3),
        "max_ms": round(max(samples), 3),
    }


def http(base, path, timeout, method="GET", body=None):
    headers = {"Accept": "application/json, application/octet-stream"}
    if body is not None:
        body = json.dumps(body, separators=(",", ":")).encode("utf-8")
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(base + path, data=body, headers=headers, method=method)
    started = time.perf_counter()
    try:
        response = urllib.request.urlopen(request, timeout=timeout)
    except urllib.error.HTTPError as exc:
        response = exc  # Return expected 409 responses as values, not exceptions.
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise BenchmarkError(f"{method} {path}: {exc}") from exc
    with response:
        data = response.read()
        status = response.status
        mime = response.headers.get("Content-Type", "").split(";", 1)[0].strip()
    elapsed = (time.perf_counter() - started) * 1000
    if mime == "application/json":
        try:
            parsed = json.loads(data)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise BenchmarkError(f"{method} {path}: invalid JSON body") from exc
        if not isinstance(parsed, dict):
            raise BenchmarkError(f"{method} {path}: response JSON is not an object")
        data = parsed
    return status, mime, data, elapsed


def json_call(base, path, timeout, method="GET", body=None, statuses=(200,)):
    code, mime, data, ms = http(base, path, timeout, method, body)
    if code not in statuses or mime != "application/json" or not isinstance(data, dict):
        raise BenchmarkError(f"{method} {path}: expected JSON status {statuses}, got {code} {mime}: {str(data)[:300]}")
    return code, data, ms


def await_state(base, path, timeout, deadline_s, interval_s, *, expected="completed"):
    started = time.perf_counter()
    polls = 0
    while True:
        _code, state, _ms = json_call(base, path, timeout)
        polls += 1
        if state.get("state") in ("completed", "failed", "canceled"):
            if state["state"] != expected:
                raise BenchmarkError(f"{path} ended in {state['state']}: {state.get('error', '')}")
            return state, round((time.perf_counter() - started) * 1000, 3), polls
        if time.perf_counter() - started >= deadline_s:
            raise BenchmarkError(f"{path} did not complete within {deadline_s}s")
        time.sleep(interval_s)


def canonical_json(data):
    return json.dumps(data, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def read_pages(base, path, timeout, repeats, concurrency):
    # Verify that sequential and simultaneous cache reads are identical.
    first_times = []
    first_result = None
    hashes = []
    for _ in range(repeats):
        _, result, ms = json_call(base, path, timeout)
        if first_result is None:
            first_result = result
        elif result != first_result:
            raise BenchmarkError("identical sequential cache reads returned different JSON")
        first_times.append(ms)
        hashes.append(hashlib.sha256(canonical_json(result)).hexdigest())

    def read_one(_):
        _, result, ms = json_call(base, path, timeout)
        return result, ms

    start = time.perf_counter()
    with ThreadPoolExecutor(max_workers=concurrency) as pool:
        concurrent = list(pool.map(read_one, range(concurrency)))
    wall_ms = (time.perf_counter() - start) * 1000
    for result, _ms in concurrent:
        if result != first_result:
            raise BenchmarkError("concurrent cache reads differed from serial response")
    return {
        "sha256_canonical_json": hashes[0],
        "response_bytes_canonical_json": len(canonical_json(first_result)),
        "sequential": summary(first_times),
        "simultaneous_clients": concurrency,
        "parallel_wall_ms": round(wall_ms, 3),
        "parallel_request_latencies": summary([ms for _, ms in concurrent]),
    }


def run(args):
    report = {
        "schema": "aowis_hydrology_derived_cache_http_v1",
        "source": "live cpp-httplib HTTP service; existing completed analysis",
        "server_url": args.url,
        "job_id": args.job_id,
        "scenario_origin": "new" if args.create_scenario else "existing",
        "true_cold_os_page_cache": False,
        "measurement": "client wall time (perf_counter), not server CPU or total process RSS",
        "repeats": args.repeats,
        "concurrent_clients": args.concurrent_clients,
    }
    _, status, _ = json_call(args.url, "/status", args.timeout)
    if status.get("status") != "ok":
        raise BenchmarkError("Hydrology /status is not ok")
    job_path = f"/v1/jobs/{args.job_id}"
    _, job, _ = json_call(args.url, job_path, args.timeout)
    if job.get("state") != "completed":
        raise BenchmarkError("job must be completed before measuring derived caches")
    report["grid"] = {"columns": job.get("columns"), "rows": job.get("rows")}
    if args.create_scenario:
        west, south, east, north = (job.get(x) for x in ("west", "south", "east", "north"))
        if not all(isinstance(x, (int, float)) and math.isfinite(x) for x in (west, south, east, north)) or east <= west or north <= south:
            raise BenchmarkError("job lacks valid WGS84 bounds for scenario creation")
        dx, dy = (east - west) * .1, (north - south) * .1
        w, e, s, n = west + dx, east - dx, south + dy, north - dy
        scenario_request = {
            "name": "Derived-cache benchmark",
            "geometry": {"type": "Polygon", "coordinates": [[[w, s], [e, s], [e, n], [w, n], [w, s]]]},
            "depth_mm": args.depth_mm,
            "duration_seconds": 3600,
            "effective_runoff_fraction": args.runoff_fraction,
        }
        _, task, submitted_ms = json_call(args.url, job_path + "/rainfall-scenario-tasks",
                                         args.timeout, method="POST", body=scenario_request,
                                         statuses=(202,))
        task_id = uuid_arg(task.get("task_id"))
        result, wait_ms, polls = await_state(args.url,
            job_path + "/rainfall-scenario-tasks/" + task_id,
            args.timeout, args.job_timeout, args.poll_interval)
        scenario = result.get("scenario", {})
        scenario_id = uuid_arg(scenario.get("id"))
        report["scenario_creation"] = {"submit_ms": round(submitted_ms, 3),
                                       "task_wait_ms": wait_ms, "polls": polls}
    else:
        scenario_id = args.scenario_id
    report["scenario_id"] = scenario_id
    path = job_path + "/rainfall-scenarios/" + scenario_id
    json_call(args.url, path, args.timeout)
    derived_path = path + "/redistribution"
    initial_status, _result, initial_ms = json_call(args.url, derived_path + "?offset=0&limit=50",
        args.timeout, statuses=(200, 409))
    report["redistribution_initial_read"] = {
        "status": initial_status, "request_ms": round(initial_ms, 3),
        "cache_present": initial_status == 200,
    }
    if initial_status == 409:
        if not args.build_missing:
            raise BenchmarkError("redistribution cache is absent; rerun with --build-missing to publish one")
        code, _started, post_ms = json_call(args.url, derived_path, args.timeout,
                                           method="POST", statuses=(200, 202))
        report["redistribution_submit_ms"] = round(post_ms, 3)
        if code == 202:
            _result, wait_ms, polls = await_state(args.url, derived_path + "/status",
                args.timeout, args.job_timeout, args.poll_interval)
            report["redistribution_wait_ms"] = wait_ms
            report["redistribution_poll_requests"] = polls
        else:
            report["redistribution_wait_ms"] = 0
            report["redistribution_poll_requests"] = 0
    report["redistribution_reads"] = read_pages(args.url,
        derived_path + "?offset=0&limit=50", args.timeout, args.repeats, args.concurrent_clients)
    return report


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--url", type=base_url, default="http://127.0.0.1:8123")
    p.add_argument("--job-id", required=True, type=uuid_arg,
                   help="existing completed analysis (no new Map terrain job is created)")
    scenarios = p.add_mutually_exclusive_group(required=True)
    scenarios.add_argument("--scenario-id", type=uuid_arg, help="reuse a saved rainfall scenario")
    scenarios.add_argument("--create-scenario", action="store_true",
                           help="explicitly create and persist one new rainfall scenario")
    p.add_argument("--build-missing", action="store_true",
                   help="explicitly schedule redistribution if cache is absent")
    p.add_argument("--repeats", type=positive_int, default=10)
    p.add_argument("--concurrent-clients", type=positive_int, default=4)
    p.add_argument("--timeout", type=positive_int, default=30)
    p.add_argument("--job-timeout", type=positive_int, default=300)
    p.add_argument("--poll-interval", type=float, default=.1)
    p.add_argument("--depth-mm", type=float, default=20.0)
    p.add_argument("--runoff-fraction", type=float, default=.5)
    return p


def main(argv=None):
    p = parser()
    args = p.parse_args(argv)
    if args.repeats > 100 or args.concurrent_clients > 64 or not (.01 <= args.poll_interval <= 10) or not (0 < args.depth_mm <= 10000) or not (0 <= args.runoff_fraction <= 1) or not all(math.isfinite(x) for x in (args.poll_interval, args.depth_mm, args.runoff_fraction)):
        p.error("repeats <= 100, concurrent-clients <= 64, poll-interval .01..10, depth-mm (0,10000], runoff-fraction [0,1] required")
    try:
        print(json.dumps(run(args), indent=2, sort_keys=True, allow_nan=False))
    except (BenchmarkError, ValueError, TypeError, KeyError) as exc:
        print(f"Derived-cache benchmark failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
