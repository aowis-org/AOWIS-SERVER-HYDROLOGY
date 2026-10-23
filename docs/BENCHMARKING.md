# Hydrology performance benchmarking and reproducibility

This is the **operating manual** for the repository's existing benchmark tools.
Observed results, comparisons, decisions and unanswered questions are kept
separately in [BENCHMARK_FINDINGS.md](BENCHMARK_FINDINGS.md). A benchmark
measures computational/service behavior, **not scientific validity** of a
flood prediction; use independent hydrological reference cases for that.

## Available benchmark tools

| Tool | Scope | What it does not measure |
| --- | --- | --- |
| `aowis-hydrology-benchmark` | Deterministic synthetic terrain, actual core D8, original-DEM hierarchy, runoff redistribution, stage RSS/timings | Map, Qt service, network, GeoTIFF, persistence, dynamic coupled routing |
| `tools/benchmark_map_http.py` | One new or saved real-Map job, repeated result reads, optional exact Map/processed-DEM window parity | Long-term throughput; `--repeats` does **not** repeat full analysis |
| `tools/benchmark_map_series.py` | Independent sequential Map-sourced jobs, worker and source-call timing distributions | Concurrent analyses |
| `tools/benchmark_map_concurrency.py` | Read-only replay of the same Map patch layout with configurable parallel GETs | Hydrology scheduling or production transport setting |
| `tools/benchmark_map_jobs.py` | Simultaneous new terrain-analysis jobs, throughput, per-job stage times, optional process RSS | Sustainable admission rate, total host memory or automatic job cleanup |

All tools generate JSON. Most real-server scripts use only the Python standard
library and require the Map and Hydrology services to be **already running**.
They do not provision Map data or start/stop server processes. Full live runs
create persisted analysis directories; the tools do **not delete** them.
Use work areas and cell limits deliberately.

## Synthetic core benchmark

Build separately, with benchmarking explicitly enabled:

```bash
cmake -S . -B build-hydrology-benchmark -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAOWIS_HYDROLOGY_BUILD_SERVER=OFF \
  -DAOWIS_HYDROLOGY_BUILD_SERVICE=OFF \
  -DAOWIS_HYDROLOGY_WITH_GDAL=OFF \
  -DAOWIS_HYDROLOGY_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=ON
cmake --build build-hydrology-benchmark --parallel
ctest --test-dir build-hydrology-benchmark -L benchmark-smoke --output-on-failure
```

Examples:

```bash
./build-hydrology-benchmark/aowis-hydrology-benchmark \
  --columns 256 --rows 256 --terrain rolling --seed 17 > benchmark-rolling.json
./build-hydrology-benchmark/aowis-hydrology-benchmark \
  --columns 1024 --rows 1024 --terrain pitted --seed 17 > benchmark-pitted.json
./build-hydrology-benchmark/aowis-hydrology-benchmark \
  --columns 512 --rows 512 --terrain flat --seed 17 > benchmark-flat.json
```

Options: `--columns`/`--rows` (16–5000, with at most 10 million combined
cells); `--terrain` (`rolling`, `pitted`, `flat`); `--seed` (unsigned 32-bit);
`--rain-mm` (0–1000); and `--runoff-fraction` (0–1). The generated grid is a
north-up 30 m WGS84/UTM zone 32N raster. Terrain is **synthetic**: slopes,
noise, depressions and plateaus deliberately exercise algorithmic paths but
do not model a particular city's topography. Height datum is unspecified.

The measured operations are `synthetic_terrain`, `d8_conditioning`,
`depression_hierarchy`, `uniform_runoff_raster`, and
`static_redistribution`. The benchmark releases its large transient D8 result
before building hierarchy to constrain process memory; this differs from
concurrent server memory usage. `measured_stages_total_ms` sums the measured
operations, not process startup. Linux `peak_process_rss_bytes` is process
VmHWM, and each `process_rss_bytes` is total process RSS **after** an
operation, not that operation's private heap. Missing RSS is `null`, not zero.

`elevation_fingerprint_fnv64` detects changed synthetic inputs using FNV-1a
of float32 bit patterns; it is not a cryptographic identifier.
`generated_runoff_m3`, `retained_m3`, direct/basin boundary outflows and
`mass_balance_error_m3` describe the event-volume accounting. The tool
rejects a residual exceeding `1e-7 × max(1, generated_runoff_m3)`.
Repeat the **whole executable**, not just a query of its report, to obtain
a distribution of independent wall-time observations.

## Live Map and Hydrology setup

Start the Map service at its configured address (commonly
`http://127.0.0.1:8122`) and Hydrology at its configured address (commonly
`http://127.0.0.1:8123`). In Hydrology's INI, set:

```ini
[server]
max_concurrent_analyses=4

[terrain]
dataset=copernicus-glo30
mode=rest
map_url=http://127.0.0.1:8122/
patch_size=256
max_concurrent_requests=4
```

These values reproduce the **four-worker** concurrency experiments in
[the findings](BENCHMARK_FINDINGS.md), not a general recommendation for small
RAM hosts. The server default is **one analysis worker**. Restart Hydrology
after configuration changes and check `/status` to verify the effective
worker and Map-request counts. A `--check-config` result does not ensure the
Map server is reachable. HTTPS URLs and the optional configured Map read key
are supported by the client; benchmark JSON never stores that key.

Use `curl -sS http://127.0.0.1:8123/status` and ensure the Map endpoint responds
before starting. If the client reports `Connection refused`, check both
service processes, listen ports and URL overrides; there is no dedicated
`compile_linux.sh` for starting Map from the Hydrology repository.

### One live job and DEM parity

```bash
./tools/benchmark_map_http.py \
  --hydrology-url http://127.0.0.1:8123 \
  --map-url http://127.0.0.1:8122 \
  --west 8.600 --south 50.650 --east 8.750 --north 50.800 \
  --resolution-m 30 --poll-seconds 0.05 --repeats 5 \
  > benchmark-map-http.json
```

That domain is 352 × 556 = 195,712 DEM cells in the recorded environment.
`--repeats 5` means **five stored-window and basin-page reads** after a
single new job, not five terrain analyses. `--existing-job UUID` reads an
existing completed job without recomputing, useful after a server restart.
`--analysis-directory` optionally estimates logical output bytes on the
local Hydrology host; it is neither allocated disk blocks nor memory.

The parity check reconstructs Map queries on the **original full-grid patch
layout**, crops them to the saved raster window, and compares samples with
the persisted Hydrology elevation values. Comparing two independently
rounded patch windows may falsely report mismatches near source pixels.
The output includes per-window SHA-256, `exact_bytes_equal`,
`cells_outside_tolerance` and `max_absolute_difference_m`. It verifies the
**sampled window**, not necessarily every DEM cell in a job. A Map request
made after a completed analysis can be warm-cache; it does not measure a
cold Map download.

### Repeated independent jobs

```bash
./tools/benchmark_map_series.py \
  --west 8.600 --south 50.650 --east 8.750 --north 50.800 \
  --resolution-m 30 --poll-seconds 0.05 \
  --source-patch-size 256 --runs 5 --repeats 1 \
  > benchmark-map-series.json
```

Use this when comparing end-to-end worker execution or a change to the
source adapter. Each run creates a **new persisted analysis**. Keep the
same dataset, region, source patch geometry, build, operating conditions and
Map cache policy in paired comparisons. A single job's worker timings are
not a stable performance distribution.

### Read-only Map request concurrency

```bash
./tools/benchmark_map_concurrency.py \
  --existing-job YOUR_COMPLETED_ANALYSIS_UUID \
  --hydrology-url http://127.0.0.1:8123 \
  --map-url http://127.0.0.1:8122 \
  --source-patch-size 256 --workers 1,2,4 \
  --rounds 5 --warmup-rounds 1 \
  --max-cells 250000 > benchmark-map-concurrency.json
```

The probe replays equivalent Map source patches, checks MIME/byte sizes and
SHA-256 equality, and varies only the client's number of simultaneous Map
requests. It **does not** change production server concurrency or benchmark
Hydrology's worker scheduling. Set `--max-cells` high enough for the chosen
saved job before submitting (the displayed 250,000-cell example is for a
small region).

### Simultaneous terrain-analysis jobs and RSS

For the recorded 775 × 1224 cell workload, run 1 and 4 simultaneous jobs
three times each, as separate waves. This creates **15 persistent jobs**:

```bash
map_pid="$(pidof aowis-server-map)"
hydrology_pid="$(pidof aowis-server-hydrology)"

./tools/benchmark_map_jobs.py \
  --west 8.510 --south 50.560 --east 8.840 --north 50.890 \
  --resolution-m 30 --jobs 1,4 --rounds 3 \
  --max-cells 1000000 --max-batch-cells 5000000 \
  --max-total-cells 15000000 --poll-seconds 0.05 \
  --rss-settle-seconds 5 \
  --hydrology-pid "$hydrology_pid" --map-pid "$map_pid" \
  > benchmark-map-jobs.json
```

`--jobs` accepts a unique subset of 1..4 including 1; `--rounds` is 1–5
and `--warmup-rounds` is 0–2. The client checks per-job,
per-wave and combined cell budgets **before** submitting jobs. Server-side
`max_cells` is a separate per-job limit; it may reject a test despite
permitted client budgets. Per-job queue wait excludes worker execution;
submit/poll and processing delays are distinct. Wave time stops when the
last job finishes, **before** post-wave RSS settling, saved-raster checks or
optional Map parity checks. The reported throughput is **batch throughput**
(completed jobs divided by batch wall time), not sustainable admission rate.

With local Linux PIDs, the tool samples `/proc/<pid>/status` RSS during a
wave and after the selected delay (`--rss-settle-seconds` 0–10, default 2).
`peak_kib` is **sampled process RSS**, not total host memory and possibly
not the instantaneous absolute peak. `settled_kib` is a delayed observation,
not proof that all allocations are freed or that there is no memory leak.
Memory numbers from different process lifetimes must not be interpreted as
an optimization without controlling baseline RSS. The benchmark rotates
wave order within a configuration to reduce order bias.

### Summary commands

```bash
jq '{configuration: .server_settings, created: .total_jobs_created,
  levels: [.results[] | {jobs,
    throughput: .throughput_jobs_per_second.median,
    wall_ms: .wall_ms.median,
    fetch_ms: .stage_timings_ms.terrain_fetch.median,
    d8_ms: .stage_timings_ms.d8_analysis.median,
    hierarchy_ms: .stage_timings_ms.depression_hierarchy.median,
    hydrology_peak_kib: .rss_kib.hydrology.peak_kib.median,
    hydrology_settled_kib: .rss_kib.hydrology.settled_kib.median
  }]}' benchmark-map-jobs.json
```

Only request RSS fields when process PIDs were provided. Reports include
all individual waves and per-job measurements so the medians can be
recomputed and outliers investigated.

## Interpretation of server timing fields

| Field | Interpretation |
| --- | --- |
| `stage_timings_ms.terrain_fetch` | Complete terrain reader phase, including Map patches and assembly |
| `stage_timings_ms.d8_analysis` | D8 conditioning and accumulation |
| `stage_timings_ms.depression_hierarchy` | Original-DEM hierarchy construction |
| `stage_timings_ms.raster_persistence` / `basin_persistence` | Persisting derived archives |
| `stage_timings_ms.total_worker` | Worker elapsed time, excluding prior queue wait |
| `stage_timings_ms.queue_wait` | Time in analysis queue before worker started |
| `terrain_fetch_details.patch_count` / `response_bytes` | Source requests and bytes received |
| `terrain_fetch_details.map_request_ms` | **Sum** of individual Map request lifetimes; overlaps make it exceed wall time |
| `terrain_fetch_details.map_request_wall_ms` | Wall span from first Map call issuance to last completed reply |
| `terrain_fetch_details.raster_copy_ms` | Copy/assembly time for received elevation patches |
| `terrain_fetch_details.first_request_ms`, `fastest_request_ms`, `slowest_request_ms` | Per-request observations, not percentiles |
| `terrain_peak_in_flight` | Peak HTTP patch calls outstanding **within that job** |

Some fields are unavailable for old jobs or custom injected readers; use
`null`, never synthetic zero. HTTP `GET /status` exposes worker/request caps.
A `terrain_fetch_details.map_request_ms` total is **not** a count of CPU
milliseconds or pure network latency. Reusing an HTTP session may reduce
connection setup but should not be called an optimization without a measured
end-to-end benefit. Persisted timing metadata is observational and does not
change DEM values.

## Reporting, limitations and regression checks

Before claiming any speedup, preserve: source commit, compiler and optimization
level, GeographicLib version, operating system, CPU/core count, RAM, Map and
Hydrology configuration, dataset/datum, bounds, target resolution, Map cache
state, concurrent host activity, warmups, repetitions, and raw per-wave JSON.
Compare **medians and variation**, not one best sample. Run configurations
in interleaved or counterbalanced order when practical. Repeat against
**different** geographic areas and distinct concurrent requests rather than
only overlapping identical DEM jobs.

Always check numerical compatibility independently of performance: grid
geometry, input fingerprints, sampled DEM parity, basin count and hierarchy
identities, mass balance and binary archive integrity. A matching sample
hash proves only that sample; it does not replace whole-grid comparison or
independent reference-model validation. Synthetic timings cannot be compared
directly with Map-sourced HTTP job times, and test-suite wall time is not a
benchmark of the analysis engine.

The default CTest suite excludes expensive live benchmarks. Offline fixture
checks are provided in `tests/test_benchmark_map_http.py`,
`test_benchmark_map_series.py`, `test_benchmark_map_concurrency.py`, and
`test_benchmark_map_jobs.py`; they do not require running servers. Benchmark
smoke tests are registered when `AOWIS_HYDROLOGY_BUILD_BENCHMARKS=ON`.

The recorded performance conclusions and restart checklist are in
[BENCHMARK_FINDINGS.md](BENCHMARK_FINDINGS.md).
