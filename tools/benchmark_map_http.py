#!/usr/bin/env python3
"""Opt-in live Map -> Hydrology HTTP benchmark; Python standard library only."""

import argparse
import hashlib
import json
import math
import pathlib
import statistics
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


class BenchmarkError(Exception):
    pass


def positive_float(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("expected a finite positive number")
    return number


def positive_int(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("expected an integer greater than zero")
    return number


def endpoint_url(base, path, parameters=None):
    url = urllib.parse.urlsplit(base)
    if url.scheme not in ("http", "https") or not url.hostname or url.username or url.password or url.query or url.fragment:
        raise BenchmarkError("server URL must be HTTP(S), with no credentials, query or fragment")
    base_path = url.path.rstrip("/")
    query = urllib.parse.urlencode(parameters or {})
    return urllib.parse.urlunsplit((url.scheme, url.netloc, base_path + path, query, ""))


def http_request(base, path, timeout, method="GET", body=None, headers=None, parameters=None):
    url = endpoint_url(base, path, parameters)
    payload = json.dumps(body, separators=(",", ":")).encode("utf-8") if body is not None else None
    request_headers = {"Accept": "application/json, application/octet-stream"}
    if payload is not None:
        request_headers["Content-Type"] = "application/json"
    if headers:
        request_headers.update(headers)
    request = urllib.request.Request(url, data=payload, headers=request_headers, method=method)
    start = time.perf_counter()
    try:
        with urllib.request.urlopen(request, timeout=timeout) as reply:
            data = reply.read()
            code = reply.status
            content_type = reply.headers.get("Content-Type", "").split(";", 1)[0].strip()
    except urllib.error.HTTPError as error:
        message = error.read(500).decode("utf-8", errors="replace")
        raise BenchmarkError(f"{method} {path} failed: HTTP {error.code}: {message}") from error
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        raise BenchmarkError(f"{method} {path} failed: {error}") from error
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    if code not in (200, 201, 202):
        raise BenchmarkError(f"{method} {path} unexpected HTTP status {code}")
    return data, content_type, elapsed_ms


def http_json(base, path, timeout, **kwargs):
    data, mime, duration = http_request(base, path, timeout, **kwargs)
    if mime != "application/json":
        raise BenchmarkError(f"{path} returned content type {mime!r}, expected application/json")
    try:
        result = json.loads(data)
    except (json.JSONDecodeError, UnicodeDecodeError) as error:
        raise BenchmarkError(f"{path} returned invalid JSON") from error
    if not isinstance(result, dict):
        raise BenchmarkError(f"{path} returned non-object JSON")
    return result, duration


def estimated_grid(bounds, resolution):
    west, south, east, north = bounds
    if not all(math.isfinite(v) for v in bounds) or not (-180 <= west < east <= 180) or not (-85 <= south < north <= 85):
        raise BenchmarkError("invalid WGS84 bounds")
    if not 30 <= resolution <= 5000:
        raise BenchmarkError("resolution_m must be between 30 and 5000")
    midlat = (north + south) * 0.5 * math.pi / 180
    columns = math.ceil((east - west) * 111195.08 * math.cos(midlat) / resolution)
    rows = math.ceil((north - south) * 111195.08 / resolution)
    if columns < 2 or rows < 2:
        raise BenchmarkError("bounds too small for the requested resolution")
    return columns, rows


def timed_summary(samples):
    return {
        "count": len(samples),
        "first_ms": round(samples[0], 3),
        "median_ms": round(statistics.median(samples), 3),
        "minimum_ms": round(min(samples), 3),
        "maximum_ms": round(max(samples), 3),
        "all_ms": [round(sample, 3) for sample in samples],
    }


def find_local_job_bytes(root, job_id):
    if root is None:
        return None
    folder = pathlib.Path(root).expanduser() / job_id
    if not folder.is_dir() or folder.is_symlink():
        raise BenchmarkError("--analysis-directory does not contain the expected job directory")
    total = 0
    count = 0
    for path in folder.rglob("*"):
        if path.is_symlink():
            raise BenchmarkError("analysis output contains a symlink; refusing filesystem measurement")
        if path.is_file():
            total += path.stat().st_size
            count += 1
    return {"files": count, "logical_bytes": total, "measurement": "local on-disk file lengths, not allocated blocks"}


def map_acquisition_patches(metadata, rect, patch_size):
    """Yield original Map source requests overlapping a persisted Hydrology window.

    Hydrology partitions the entire analysis raster into patch_size blocks. A
    smaller independent Map request can change float cell-centre arithmetic and
    select a neighbouring source DEM pixel on an exact pixel boundary.
    """
    columns, rows = metadata["columns"], metadata["rows"]
    west = float(metadata["west"])
    north = float(metadata["north"])
    dx = float(metadata["step_lon_deg"])
    dy = float(metadata["step_lat_deg"])
    x, y, width, height = (rect[name] for name in ("x", "y", "width", "height"))
    for patch_y in range((y // patch_size) * patch_size, y + height, patch_size):
        patch_rows = min(patch_size, rows - patch_y)
        for patch_x in range((x // patch_size) * patch_size, x + width, patch_size):
            patch_columns = min(patch_size, columns - patch_x)
            patch_west = west + patch_x * dx
            patch_east = patch_west + patch_columns * dx
            patch_north = north - patch_y * dy
            patch_south = patch_north - patch_rows * dy
            params = {
                "dataset": metadata["dataset"],
                "west": format(patch_west, ".17g"),
                "south": format(patch_south, ".17g"),
                "east": format(patch_east, ".17g"),
                "north": format(patch_north, ".17g"),
                "columns": patch_columns,
                "rows": patch_rows,
            }
            yield patch_x, patch_y, patch_columns, patch_rows, params


def fetch_original_map_window(base, timeout, headers, metadata, rect, patch_size):
    """Query the same source patches as Hydrology, then crop the requested cells."""
    x, y, width, height = (rect[name] for name in ("x", "y", "width", "height"))
    sample = bytearray(width * height * 4)
    total_ms = 0.0
    total_bytes = 0
    requests = 0
    for px, py, pw, ph, params in map_acquisition_patches(metadata, rect, patch_size):
        data, mime, elapsed = http_request(base, "/terrain/v1/analysis/grid", timeout,
                                           headers=headers, parameters=params)
        if not mime.startswith("application/vnd.aowis.dem-f32le") or len(data) != pw * ph * 4:
            raise BenchmarkError("Map DEM source patch MIME or byte count mismatch")
        requests += 1
        total_ms += elapsed
        total_bytes += len(data)
        left, right = max(x, px), min(x + width, px + pw)
        top, bottom = max(y, py), min(y + height, py + ph)
        for source_y in range(top, bottom):
            source_offset = ((source_y - py) * pw + left - px) * 4
            target_offset = ((source_y - y) * width + left - x) * 4
            length = (right - left) * 4
            sample[target_offset:target_offset + length] = data[source_offset:source_offset + length]
    return bytes(sample), total_ms, total_bytes, requests


def parity_differences(map_bytes, hydro_bytes, rect, tolerance):
    mismatches = 0
    maximum_difference = 0.0
    examples = []
    width = rect["width"]
    for index, ((map_z,), (hyd_z,)) in enumerate(zip(
            struct.iter_unpack("<f", map_bytes), struct.iter_unpack("<f", hydro_bytes))):
        if not math.isfinite(map_z) or not math.isfinite(hyd_z):
            raise BenchmarkError("Map or Hydrology sample includes nonfinite elevation")
        difference = abs(map_z - hyd_z)
        maximum_difference = max(maximum_difference, difference)
        if difference > tolerance:
            mismatches += 1
            if len(examples) < 8:
                examples.append({"x": rect["x"] + index % width,
                                 "y": rect["y"] + index // width,
                                 "map_m": map_z, "hydrology_m": hyd_z,
                                 "difference_m": difference})
    return mismatches, maximum_difference, examples


def run(args):
    if args.existing_job is None:
        if None in (args.west, args.south, args.east, args.north):
            raise BenchmarkError("new jobs require --west --south --east --north")
        dimensions = estimated_grid((args.west, args.south, args.east, args.north), args.resolution_m)
        if dimensions[0] * dimensions[1] > args.max_cells:
            raise BenchmarkError(f"estimated {dimensions[0] * dimensions[1]} cells exceeds --max-cells={args.max_cells}; no HTTP job submitted")
    else:
        dimensions = None
        if not (len(args.existing_job) == 36 and all(c in "0123456789abcdefABCDEF-" for c in args.existing_job)):
            raise BenchmarkError("--existing-job must be a UUID without braces")

    # Only supply a Map key on requests to Map, never to Hydrology or in the report.
    map_headers = {"X-API-Key": args.map_read_api_key} if args.map_read_api_key else {}
    report = {
        "schema_version": 1,
        "benchmark": "live_map_hydrology_http",
        "hydrology_url": urllib.parse.urlunsplit(urllib.parse.urlsplit(args.hydrology_url)._replace(query="", fragment="")),
        "map_url": urllib.parse.urlunsplit(urllib.parse.urlsplit(args.map_url)._replace(query="", fragment="")) if args.map_url else None,
        "mode": "existing_job" if args.existing_job else "new_job",
        "request_resolution_m": args.resolution_m if dimensions is not None else None,
        "estimated_columns": dimensions[0] if dimensions else None,
        "estimated_rows": dimensions[1] if dimensions else None,
        "poll_interval_s": args.poll_seconds,
        "request_timeout_s": args.request_timeout,
        "timing_clock": "time.perf_counter, client wall time",
    }

    health, health_ms = http_json(args.hydrology_url, "/status", args.request_timeout)
    if health.get("status") != "ok":
        raise BenchmarkError("Hydrology server /status is not ok")
    report["hydrology_status_ms"] = round(health_ms, 3)

    if args.existing_job:
        job_id = args.existing_job
        metadata, first_read_ms = http_json(args.hydrology_url, "/v1/jobs/" + job_id, args.request_timeout)
        report["existing_job_first_metadata_ms"] = round(first_read_ms, 3)
    else:
        body = {
            "west": args.west, "south": args.south, "east": args.east, "north": args.north,
            "resolution_m": args.resolution_m, "dataset": args.dataset,
            "fill_depressions": args.fill_depressions,
        }
        metadata, submit_ms = http_json(args.hydrology_url, "/v1/jobs", args.request_timeout,
                                        method="POST", body=body)
        job_id = metadata.get("id")
        if not isinstance(job_id, str) or not job_id:
            raise BenchmarkError("Hydrology did not return a job ID")
        report["submit_request_ms"] = round(submit_ms, 3)
    report["job_id"] = job_id
    job_path = "/v1/jobs/" + urllib.parse.quote(job_id, safe="")

    status_events = []
    last_state = None
    poll_count = 0
    start = time.perf_counter()
    while True:
        state = metadata.get("state")
        if state != last_state:
            status_events.append({"state": state, "observed_elapsed_s": round(time.perf_counter() - start, 3)})
            last_state = state
        if state in ("completed", "failed", "canceled"):
            break
        if time.perf_counter() - start >= args.job_timeout:
            raise BenchmarkError(f"analysis {job_id} exceeded {args.job_timeout}s; job remains running")
        time.sleep(args.poll_seconds)
        metadata, _ = http_json(args.hydrology_url, job_path, args.request_timeout)
        poll_count += 1
    if state != "completed":
        raise BenchmarkError(f"analysis {job_id} ended in state {state!r}: {metadata.get('error', '')}")
    report["job_wait_s"] = round(time.perf_counter() - start, 3)
    report["status_transition_observations"] = status_events
    report["status_poll_requests"] = poll_count
    columns, rows = metadata.get("columns"), metadata.get("rows")
    if not (isinstance(columns, int) and isinstance(rows, int) and columns > 0 and rows > 0):
        raise BenchmarkError("completed job contains invalid raster dimensions")
    if columns * rows > args.max_cells:
        raise BenchmarkError(f"stored job has {columns*rows} cells; exceeds client --max-cells, refusing reads")
    report["raster_columns"] = columns
    report["raster_rows"] = rows
    report["cells"] = columns * rows
    report["basin_count"] = metadata.get("basin_count")
    report["conditioned_cells"] = metadata.get("conditioned_cells")
    stage_timings = metadata.get("stage_timings_ms")
    if stage_timings is not None and not (isinstance(stage_timings, dict) and
        all(isinstance(value, (int, float)) and not isinstance(value, bool) and
            math.isfinite(value) and value >= 0 for value in stage_timings.values())):
        raise BenchmarkError("Hydrology returned invalid server stage timings")
    report["server_stage_timings_ms"] = stage_timings
    details = metadata.get("terrain_fetch_details")
    if details is not None:
        numeric_fields = ("patch_count", "response_bytes", "map_request_ms", "raster_copy_ms",
                          "first_request_ms", "fastest_request_ms", "slowest_request_ms")
        if (not isinstance(details, dict) or
            details.get("transport") not in ("map_http", "map_in_process") or
            any(not isinstance(details.get(field), (int, float)) or
                isinstance(details.get(field), bool) or
                not math.isfinite(details[field]) or details[field] < 0
                for field in numeric_fields) or
            not float(details["patch_count"]).is_integer() or
            not float(details["response_bytes"]).is_integer() or
            details["patch_count"] < 1 or
            details["fastest_request_ms"] > details["slowest_request_ms"] or
            details["first_request_ms"] > details["slowest_request_ms"] or
            details["first_request_ms"] < details["fastest_request_ms"]):
            raise BenchmarkError("Hydrology returned invalid terrain fetch details")
    report["server_terrain_fetch_details"] = details
    report["source_dataset"] = metadata.get("dataset")
    report["vertical_datum"] = metadata.get("vertical_datum")

    x = max(0, (columns - args.window) // 2)
    y = max(0, (rows - args.window) // 2)
    width, height = min(columns - x, args.window), min(rows - y, args.window)
    rect = {"x": x, "y": y, "width": width, "height": height}
    report["raster_window"] = rect
    raster = None
    times = []
    for _ in range(args.repeats):
        payload, mime, elapsed = http_request(args.hydrology_url,
            job_path + "/raster/elevation", args.request_timeout, parameters=rect)
        if mime != "application/vnd.aowis.raster-f32le" or len(payload) != width * height * 4:
            raise BenchmarkError("Hydrology elevation window MIME or byte count mismatch")
        if raster is not None and payload != raster:
            raise BenchmarkError("repeated elevation window responses disagree")
        raster = payload
        times.append(elapsed)
    report["elevation_window"] = timed_summary(times)
    report["elevation_window"]["bytes"] = len(raster)
    report["elevation_window"]["sha256"] = hashlib.sha256(raster).hexdigest()

    query_times = []
    basin_page = None
    for _ in range(args.repeats):
        page, elapsed = http_json(args.hydrology_url, job_path + "/basins", args.request_timeout,
                                  parameters={"offset": 0, "limit": args.basin_limit})
        if basin_page is not None and page != basin_page:
            raise BenchmarkError("repeated basin page responses disagree")
        basin_page = page
        query_times.append(elapsed)
    report["basin_page"] = timed_summary(query_times)
    report["basin_page"]["limit"] = args.basin_limit
    report["basin_page"]["response_bytes"] = len(json.dumps(basin_page).encode("utf-8"))
    report["analysis_files"] = find_local_job_bytes(args.analysis_directory, job_id)

    if args.map_url:
        map_times = []
        map_first = None
        map_transferred = None
        map_patch_requests = None
        for _ in range(args.repeats):
            payload, duration, transferred, requests = fetch_original_map_window(
                args.map_url, args.request_timeout, map_headers, metadata, rect,
                args.source_patch_size)
            if map_first is not None and payload != map_first:
                raise BenchmarkError("repeated Map DEM source windows disagree")
            map_first = payload
            map_transferred = transferred
            map_patch_requests = requests
            map_times.append(duration)
        mismatches, max_difference, examples = parity_differences(
            map_first, raster, rect, args.elevation_tolerance_m)
        report["map_hydrology_parity"] = {
            "map_sample_requests": timed_summary(map_times),
            "map_requests_per_repeat": map_patch_requests,
            "map_transferred_bytes_per_repeat": map_transferred,
            "map_response_bytes": len(map_first),
            "map_sha256": hashlib.sha256(map_first).hexdigest(),
            "hydrology_sha256": hashlib.sha256(raster).hexdigest(),
            "exact_bytes_equal": map_first == raster,
            "max_absolute_difference_m": max_difference,
            "cells_outside_tolerance": mismatches,
            "tolerance_m": args.elevation_tolerance_m,
            "source_patch_size": args.source_patch_size,
            "mismatch_examples": examples,
            "comparison": "Map sampled using Hydrology's original terrain patch layout, cropped to the saved raster window",
        }
        if mismatches:
            raise BenchmarkError(
                f"Map/Hydrology elevation parity failed on {mismatches} cells "
                f"(max difference {max_difference:.6g} m; first differences "
                f"{json.dumps(examples)}; analysis job {job_id}). "
                "Verify --source-patch-size matches Hydrology terrain/patch_size "
                "and both services use the same Map terrain source.")

    report["notes"] = [
        "Client job wait includes polling and queue delays; server_stage_timings_ms measures individual worker stages when available.",
        "Queries are sequential client-measured requests. OS caches and Map cache may already be warm.",
        "Map sample is requested after the Hydrology job; it does not measure a cold Map download.",
        "Synthetic core benchmark and real HTTP benchmark are not directly comparable.",
        "Analysis directory byte count is optional, not server memory usage or disk blocks.",
    ]
    return report


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hydrology-url", default="http://127.0.0.1:8123")
    parser.add_argument("--map-url", default="http://127.0.0.1:8122",
                        help="Map endpoint for independent DEM comparison; set '' to skip")
    parser.add_argument("--map-read-api-key", default="", help="Map read key; omitted from report")
    parser.add_argument("--existing-job", help="Existing analysis UUID for cache/restart benchmark")
    for direction in ("west", "south", "east", "north"):
        parser.add_argument("--" + direction, type=float)
    parser.add_argument("--resolution-m", type=positive_float, default=30.0)
    parser.add_argument("--dataset", default="copernicus-glo30")
    parser.add_argument("--fill-depressions", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--max-cells", type=positive_int, default=250000,
                        help="Safety budget for live analysis and existing-job queries")
    parser.add_argument("--window", type=positive_int, default=64, help="Window dimension (max 512)")
    parser.add_argument("--source-patch-size", type=positive_int, default=256,
                        help="Hydrology terrain/patch_size used when the original DEM was acquired (default 256)")
    parser.add_argument("--basin-limit", type=positive_int, default=25, help="Basin page size (max 100)")
    parser.add_argument("--repeats", type=positive_int, default=5)
    parser.add_argument("--poll-seconds", type=positive_float, default=0.25)
    parser.add_argument("--job-timeout", type=positive_float, default=600.0)
    parser.add_argument("--request-timeout", type=positive_float, default=180.0)
    parser.add_argument("--elevation-tolerance-m", type=positive_float, default=0.0001)
    parser.add_argument("--analysis-directory", help="Optional local Hydrology analysis output root, for logical disk bytes")
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.window > 512 or args.source_patch_size > 512 or args.basin_limit > 100 or args.repeats > 100 or args.poll_seconds > 60:
        parser.error("--window <=512, --source-patch-size <=512, --basin-limit <=100, --repeats <=100, --poll-seconds <=60 required")
    try:
        result = run(args)
    except (BenchmarkError, KeyError, ValueError, TypeError) as error:
        print(f"Live benchmark failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
