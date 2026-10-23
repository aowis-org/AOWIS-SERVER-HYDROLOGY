# Hydrology benchmarking: synthetic core and live Map integration

## Purpose and limits

This guide provides two independent, repeatable benchmark workflows: a seeded
**synthetic C++ core benchmark** and a **live Map → Hydrology HTTP benchmark**.
Neither is a claim that
synthetic elevations reproduce Copernicus GLO-30, nor a validation of predicted
flooding. The opt-in `aowis-hydrology-benchmark` runs actual core algorithms in
one process and reports timing, memory, algorithmic counts, and conservation
checks in **machine-readable JSON**.

The measured workflow is:

1. Generate a north-up, synthetic 30 m UTM 32N raster.
2. Run `analyze()` with the default depression-filling policy (physical
   Priority-Flood, D8 routing, upstream accumulation and fill diagnostics).
3. Release the transient analysis result, retaining the immutable input raster.
4. Build the original-DEM `DepressionHierarchy` with merge saddles/capacities.
5. Generate a uniform, area-aware per-cell effective runoff-volume raster.
6. Call `redistributeRunoff()` using the original DEM, without reusing the
   conditioned D8 elevations, then **verify mass conservation**.

The synthetic stages call production C++ core code. Neither Map network acquisition,
GeoTIFF decoding, Stage 7 polygon clipping, SQLite/PostgreSQL, asynchronous
Qt services, HTTP, persistent caches, nor the coupled transient reservoir
solver is included. Measure those independently, using representative inputs.

The *synthetic* vertical datum is unknown. `EPSG:32632` establishes only the
horizontal grid. Raster geometry is 30 m × 30 m in projected coordinates;
GeographicLib is used to correct row-cell areas for the UTM projection scale.

## Building

From `AOWIS-SERVER-HYDROLOGY`:

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

GeographicLib must already be initialized via `tools/setup_geographiclib.sh`
or supplied with `AOWIS_HYDROLOGY_GEOGRAPHICLIB_SOURCE`. Qt and GDAL are not
required for this benchmark-only build. The ordinary `compile_linux_tests.sh
--both` workflow is unchanged: **benchmarks are disabled by default**. When
enabled with CTest, two small regression checks exercise the report generator
and rejection of inputs exceeding the cell budget. If Python 3 is installed,
an additional offline HTTP-protocol fixture verifies the live benchmark
client without requiring either server. No million-cell or live-network test
is added to CI by default.

## Workloads

```bash
# Development baseline (65,536 cells).
./build-hydrology-benchmark/aowis-hydrology-benchmark \
  --columns 256 --rows 256 --terrain rolling --seed 17 > hyd-256.json

# Small district-sized exercise (1,048,576 cells).
./build-hydrology-benchmark/aowis-hydrology-benchmark \
  --columns 1024 --rows 1024 --terrain pitted --seed 17 > hyd-1024.json

# ~3,000 × 3,000 30 m cells, a synthetic ~90 × 90 km domain.
# This can require substantial RAM; run on a dedicated test machine.
./build-hydrology-benchmark/aowis-hydrology-benchmark \
  --columns 3000 --rows 3000 --terrain rolling --seed 17 > hyd-3000.json

# Stress the flat-routing path.
./build-hydrology-benchmark/aowis-hydrology-benchmark \
  --columns 512 --rows 512 --terrain flat --seed 17 > hyd-flat.json
```

Options (all explicit, with no random global state):

| Argument | Default | Supported values |
| --- | --- | --- |
| `--columns`, `--rows` | `256`, `256` | 16–5000 each; maximum **10 million** cells in total |
| `--seed` | `1` | unsigned 32-bit integer |
| `--terrain` | `rolling` | `rolling`, `pitted`, `flat` |
| `--rain-mm` | `35` | 0–1000 mm, uniformly over the full raster |
| `--runoff-fraction` | `0.65` | 0–1 |
| `--help` | — | usage and limits |

`rolling` has a large-scale slope, low-frequency sinusoidal corrugations and
seeded high-frequency noise; it intentionally stresses handling of many
small local minima. `pitted` adds deep rectangular negative-elevation offsets
to that terrain; `flat` is a constant-height plateau. **None is a measured
geographical DEM or a statistical surrogate for a particular city.** The
fixed counter-based noise generator makes a particular raster repeatable on
the same software/platform. Floating-point math and optimization settings can
produce different last bits across architectures.

## Report format and interpretation

Standard output contains exactly one JSON document on success; errors are
written to standard error with nonzero exit status. Each document includes:

| Field | Meaning |
| --- | --- |
| `schema_version` | JSON report schema, currently `1` |
| `benchmark`, `terrain`, `seed`, `cells`, `crs`, `cell_size_m` | Workload identity |
| `elevation_fingerprint_fnv64` | FNV-1a of little-endian *float32 bit patterns*, to detect changed synthetic DEMs; **not** a cryptographic checksum |
| `conditioned_cells` | Number of physically modified fill cells in `analyze()` |
| `basin_count` | Number of leaf and parent depression-hierarchy nodes |
| `generated_runoff_m3`, `retained_m3`, `total_boundary_outflow_m3` | Original-D8 static storm volume accounting |
| `mass_balance_error_m3` | Floating-point residual; failure if outside `1e-7 × max(1, generated_runoff)` |
| `stages[].name`, `stages[].wall_ms` | Per-operation elapsed wall time, **one run**, not a statistical median |
| `stages[].process_rss_bytes` | Total process resident memory after a stage, on Linux; not stage-private allocation |
| `peak_process_rss_bytes` | Linux `/proc/self/status` VmHWM peak RSS of the process; `null` elsewhere |
| `measured_stages_total_ms` | Sum of measured stage wall times, not end-to-end process launch time |

The original DEM remains resident during the workflow. The large
`AnalysisResult` is deliberately discarded before basin construction to keep
the benchmark's peak closer to a streaming pipeline; the service's actual
memory profile may differ. Per-stage RSS includes other allocations and is
not a replacement for heap profiling. Linux VmHWM is a process metric; it
includes GeographicLib and runtime overhead. `null` means unavailable, **not
zero RAM usage**.

A reproducible report requires recording the compiler, compiler flags, build
type, CPU model, core count, system memory, OS, GeographicLib revision,
source Git commit, thread load and benchmark command. Save these outside the
JSON when comparing environments. Run multiple fresh processes (for example,
5–10), compare medians/dispersion, and retain the raw JSON outputs. Do not
interpret a single wall-time difference smaller than operating-system noise
as an optimization.

## Hierarchy row-area optimization: measured results

A real Copernicus GLO-30 job with 352 × 556 cells (195,712 cells) was
measured on the user's Linux server **before** optimization. The server
reported `depression_hierarchy = 688.21 ms`, compared with
`terrain_fetch = 222.69 ms`, `d8_analysis = 92.22 ms`, and
`total_worker = 1037.69 ms`. This is one field measurement with already
available terrain; it is not a cold-cache distribution or a representative
metropolitan benchmark.

A source-code review identified repeated calls to
`rasterRowCellAreaM2()` **for every activated cell** in the hierarchy, even
though area depends on raster row under the supported north-up geometry.
The implementation now evaluates the **same** GeographicLib-backed area
once per row, reusing the value across cells. Bounded storage queries lazily
cache the corresponding row areas. This does not change the cell-area
formula, grouping of plateaus, parent–child relationships, or persistent
basin format.

On a separate local Release build (1024 × 1024, `pitted`, seed 17,
GeographicLib 2.7), hierarchy timing changed from about **1273 ms to
653 ms**. Five additional paired 512 × 512 synthetic seeds retained
identical basin counts, runoff totals, boundary outflows, input fingerprints,
and reported mass-balance residuals, while delivering a median hierarchy
speedup of approximately **2.08×**. These measurements are host-specific;
small fluctuations in other stage times and process RSS are not evidence
of an algorithmic regression.

On the user's Arch Linux service with the *same* 352 × 556 GLO-30 window,
the row-area optimization reduced server-side hierarchy time from **688.21 ms**
to **146.06 ms** (78.8% less). Total worker time dropped from **1037.69 ms**
to **530.12 ms** (48.9% less). The run retained **4,487 basins**,
**14,757 conditioned cells** and identical saved elevation-window SHA-256.
Terrain acquisition stayed approximately unchanged at **222.69 → 225.26 ms**.
D8 time changed from **92.22 → 112.00 ms** in these single runs; that is not
enough sampling to diagnose a D8 regression. Map elevations agreed with the
persisted raster byte-for-byte on the comparison window. These results are
**paired single-job measurements**, not a multi-run performance distribution
or proof that all hierarchy outputs match across all DEM inputs.

## Reuse the Map HTTP session within each terrain job

After the hierarchy improvement, terrain fetching became the largest measured
server stage: 225.26 ms out of 530.12 ms. The REST terrain reader acquires
bounded Map analysis-grid patches, with 1–4 outstanding requests per analysis
worker (default 4); a typical 352 × 556 raster with `terrain/patch_size=256`
uses **six source patches**. Previously, each
patch constructed a new `QNetworkAccessManager`, so Map could not reuse the
same HTTP connection across successive requests. The reader now constructs
one manager in the caller's worker thread for the duration of **one terrain
acquisition**, letting Qt reuse its connection(s) for subsequent patches.

All request bounds, columns, rows, source-precision sample values, MIME and
length checks, request timeouts, authentication headers, patch ordering and
cancellation checks are unchanged. The in-process `AOWIS::Map` path uses the
same patch geometry but does not make HTTP requests. The network manager is
**not shared between jobs or threads**. A loopback HTTP/1.1 regression test
checks multi-patch decoding, exact cell placement, a persistent connection and
pre-request cancellation.

The change reduces avoidable connection setup but does not remove work from
Map's DEM sampling implementation. It is therefore a **hypothesis about
latency**, not a measured speedup. Benchmark with a restarted Hydrology server
and the **same** `benchmark_map_http.py` invocation used for the baseline;
compare `server_stage_timings_ms.terrain_fetch` and the terrain window SHA-256.
For a robust estimate, repeat several new jobs after accounting for Map's
source cache and OS load. Client-side `map_sample_requests` is a separate
benchmark of Map's sampling endpoint, not Hydrology's connection reuse.

## Per-job Map patch diagnostics

The live 352 × 556 GLO-30 comparison following Qt HTTP session reuse
produced `terrain_fetch = 288.34 ms` and `total_worker = 559.58 ms`, compared
with `225.26 ms` and `530.12 ms` respectively in the earlier run. All 18
Hydrology regression tests passed in Debug and Release and the DEM comparison
remained byte-identical. **Connection reuse has therefore not demonstrated a
speedup on this host.** One unpaired timing per build is insufficient to
attribute the difference to Qt or Map; Map tile processing and machine load
have not been held constant. Retain connection reuse for its predictable
per-job connection lifetime, not as a proven performance optimization.

To distinguish Map request cost from Hydrology raster assembly, new terrain
jobs also publish `terrain_fetch_details` in `GET /v1/jobs/{id}` and their
persisted `job.json`. The Python live benchmark copies this object verbatim
to `server_terrain_fetch_details`. It is an additive metadata field, absent
for older jobs and for a caller-supplied `ServiceConfig::terrain_reader`.

Illustrative shape (the numbers below are *not* field measurements):

```json
"terrain_fetch_details": {
  "transport": "map_http",
  "patch_count": 6,
  "response_bytes": 782848,
  "map_request_ms": 210.0,
  "map_request_wall_ms": 120.0,
  "peak_in_flight": 4,
  "raster_copy_ms": 2.0,
  "first_request_ms": 70.0,
  "fastest_request_ms": 20.0,
  "slowest_request_ms": 70.0
}
```

| Field | Meaning |
| --- | --- |
| `transport` | `map_http` for REST, `map_in_process` for a borrowed Map object |
| `patch_count` | Number of successful Map analysis-grid patches assembled |
| `response_bytes` | Total raw float32 sample bytes (excluding HTTP headers); identical for the two modes on equal grids |
| `map_request_ms` | **Sum of per-patch Map call wall durations**. With concurrent HTTP requests it can exceed `stage_timings_ms.terrain_fetch` because requests overlap; it is **not** network-only latency |
| `map_request_wall_ms` | Elapsed span from the first Map call dispatch to the last successful reply completion, including any overlap; used for wall-clock attribution |
| `peak_in_flight` | Largest number of simultaneous outstanding Map requests (1 for sequential or in-process transport, at most `terrain/max_concurrent_requests` for REST) |
| `raster_copy_ms` | Time spent decoding/copying the returned float values into the full DEM |
| `first_request_ms` | First successful Map patch call duration |
| `fastest_request_ms`, `slowest_request_ms` | Extremal per-patch Map call durations, not percentiles |

When `peak_in_flight == 1`, the sum of `map_request_ms` and `raster_copy_ms`
is normally bounded by `stage_timings_ms.terrain_fetch`. For parallel jobs,
**this inequality no longer holds**: Map requests overlap and their summed
lifetimes are not elapsed duration. Instead compare `map_request_wall_ms` with
`stage_timings_ms.terrain_fetch`; the former includes any decoding or copying
performed *between* replies and is a span, not a separately measured CPU cost.
Do not subtract parallel per-request sums from worker wall time. This is **not** a decomposition
of Map's internal processing time versus TCP latency: that would require
additional Map-side instrumentation. The diagnostics use steady-clock wall
time, are local to each worker, and have constant memory overhead regardless
of number of patches. They do not retain coordinates, full payloads or
per-patch logs, and do not alter grid values or requests. Both HTTP and the
standalone in-process Map route report the same fields.

Compare repeated *new* jobs with the same bounds/resolution, identical Map
cache policy, and no unrelated load. Because `terrain_fetch_details` and
`stage_timings_ms` are persisted, they can also be retrieved by
`--existing-job` after restarting Hydrology. Measure multiple independent
runs and report medians and spread, rather than comparing individual jobs.

### Repeated new-job terrain acquisition measurements

`tools/benchmark_map_series.py` automates this exact methodology using only
Python 3's standard library. Unlike `benchmark_map_http.py --repeats 5`
(which times **five cached result queries from one analysis job**),
`--runs 5` creates **five independent Hydrology analysis jobs** and gathers
five server-side terrain-fetch and worker timing observations. It retains the
full per-job JSON reports, job IDs, source hashes and water-analysis counts.
The runner neither removes persisted jobs nor restarts Map/Hydrology.

Run this **after** compiling and restarting the instrumented Hydrology server
and ensuring both ports 8122 (Map) and 8123 (Hydrology) are available:

```bash
./tools/benchmark_map_series.py \
  --hydrology-url http://127.0.0.1:8123 \
  --map-url http://127.0.0.1:8122 \
  --west 8.600 --south 50.650 \
  --east 8.750 --north 50.800 \
  --resolution-m 30 --source-patch-size 256 \
  --poll-seconds 0.05 --runs 5 --repeats 1 \
  > benchmark-map-series.json

# Compact summary. This extracts server-side worker measurements, not a
# replay of the Map endpoint from the benchmark's parity check.
jq '{
  jobs: .measured_jobs, patches: .patch_count_per_job,
  fetch_ms: .timing_distributions_ms.terrain_fetch,
  map_calls_ms: .timing_distributions_ms.map_request_ms,
  map_elapsed_ms: .timing_distributions_ms.map_request_wall_ms,
  peak_in_flight: .timing_distributions_ms.peak_in_flight,
  copy_ms: .timing_distributions_ms.raster_copy_ms,
  residual_ms: .timing_distributions_ms.fetch_unattributed_ms,
  map_share_pct: .share_distributions_pct.request_of_fetch_pct,
  worker_ms: .timing_distributions_ms.total_worker
}' benchmark-map-series.json
```

The output includes `timing_distributions_ms`,
`share_distributions_pct`, and a `jobs` array containing each original
report. Summaries provide each ordered sample list, min, median, mean and
nearest-rank p90 (descriptive only: five jobs are not enough for a reliable
p90 estimate). `fetch_unattributed_ms` is the measured `terrain_fetch` minus the
Map-request elapsed **span**, and (only for sequential reads) raster-copy
wall time. It can include Qt event-loop setup, patch geometry and other
worker overhead. In parallel mode the span already includes raster assembly
between responses, so the copy duration is not subtracted again.
`request_of_fetch_pct` is the **elapsed request span** divided by fetch wall
time; `overlap_ratio` is the sum of request durations divided by their elapsed
span (typically greater than 1 during real overlap). The saved
`map_request_ms` field continues to mean the sum of individual request
lifetimes even when requests overlap. **It is not TCP latency.** Per-run
percentages are summarized from individual run ratios, not ratios of
rounded median timings.

Run identity is enforced: the full-raster dimensions, dataset, basin count,
conditioned-cell count, and queried elevation-window SHA-256 must agree
across measured jobs. Reusing a job ID, corrupt server timings, incompatible
patch metrics, and Map/Hydrology parity failures (when `--map-url` is enabled) abort the series rather than
quietly producing misleading averages. The tool needs the server's
`stage_timings_ms` and `terrain_fetch_details`; older jobs or an old server
are not sufficient. `--source-patch-size` must match `terrain/patch_size`.
Preflight `--max-cells` applies to **each** job; `--runs` is bounded to
2–20 and optional `--warmup-runs` to 0–5. **Every warmup creates another
persisted analysis** but its data are not included in the distribution.
The default is no warmup. Use `--inter-run-seconds 0.5` to separate runs
when desired (maximum 30 s). If the series fails, previously created jobs
remain available; the error names the failed run. There is no claim that
these runs measure cold Map data: source-tile and OS caches can already be
warm, even on the first job.

Interpretation: if `map_request_wall_ms` takes nearly all of `terrain_fetch`,
further Hydrology raster-copy microoptimizations cannot materially improve
acquisition time. To distinguish Map's DEM sampling CPU from HTTP delivery,
we must instrument **AOWIS-SERVER-MAP** separately; the present service
measurements cannot distinguish those two costs.


### Map HTTP concurrency probe (read-only)

Repeated job measurements on one 352 × 556 Copernicus GLO-30 area, with six
source patches, show that approximately **99.85% of Hydrology's terrain-fetch
wall time** occurs inside the six Map HTTP requests. Copying returned samples
into the DEM takes approximately 0.13 ms. That request measurement includes
**Map processing, Qt/HTTP delivery, and client waiting together**; it does
not establish whether TCP or raster sampling dominates inside Map.

Use `tools/benchmark_map_concurrency.py` to independently measure how well
the *existing* Map server serves concurrent DEM patch GET requests. This is a
**read-only experiment**: provide an already completed Hydrology job, and the
probe reconstructs the original full-grid Map patch requests from the saved
analysis geometry. It does not create jobs, delete caches, change Map settings,
or modify Hydrology's production concurrency setting.

```bash
./tools/benchmark_map_concurrency.py \
  --existing-job <completed-analysis-uuid> \
  --hydrology-url http://127.0.0.1:8123 \
  --map-url http://127.0.0.1:8122 \
  --source-patch-size 256 \
  --workers 1,2,4 --rounds 5 --warmup-rounds 1 \
  > benchmark-map-concurrency.json

jq '{patches: .patch_count, results: [.results[] | {
  workers, speedup: .speedup_vs_sequential_median,
  wall_ms: .wall.median_ms, sum_http_ms: .sum_http.median_ms
}]}' benchmark-map-concurrency.json
```

For the 352 × 556 example, the probe reads six source patches per pass.
Each worker count runs one warmup plus five measured passes, for 108 Map
requests in total (6 patches × 3 worker levels × 6 passes). Avoid running
this on a busy or production Map server; parallel sampling may increase
CPU, memory pressure, and unrelated users' latency. Select lower concurrency
or fewer rounds when needed. The default --max-cells is 250,000; worker
levels are limited to 1–8, with 1 required as baseline. Round order is
rotated to reduce systematic cache/time bias.

Every received patch must have the declared MIME and byte count, and its
SHA-256 must remain **exactly identical** across all warmups, rounds and
concurrency levels, otherwise the probe fails. Metrics distinguish:

- `wall_ms`: elapsed time to obtain the entire set of source patches;
- `sum_http_ms`: sum of individual Map GET durations (may exceed wall time
  under overlap);
- `speedup_vs_sequential_median`: sequential wall-time median divided by
  each level's median, descriptive rather than a guarantee of speedup.

Concurrent requests may be serviced in parallel by Map, but faster results
here does **not** guarantee identical gains in Hydrology's already-parallel
production reader: Python and Qt have different connection handling and
scheduling. Production `terrain_fetch` timings and byte-identical raster
outputs are the appropriate validation. Map's internal sampling CPU and
transport costs still require instrumentation in AOWIS-SERVER-MAP.

**Repository packaging:** `.gitignore` ignores `benchmark*.json` and
`benchmark*.log` result files, but not `tools/benchmark_map_series.py` or
`tools/benchmark_map_concurrency.py`. The source scripts must appear in
`tools/zip_hydrology.sh` repository archives even when untracked. All
`tools/benchmark_map_*.py` entrypoints must be executable; the packaging tool
rejects missing execute bits. Generated Python `__pycache__` directories and
bytecode files are excluded from source archives.


### Simultaneous Hydrology analysis jobs (live scaling benchmark)

`tools/benchmark_map_jobs.py` tests **inter-job** scaling rather than the
number of Map patches dispatched **within one job**. It submits several
independent jobs at approximately the same time using a bounded worker pool,
and polls each job until completion. Each job may itself dispatch up to four
parallel Map HTTP requests; thus four active jobs can ask Map to handle up to
**16** outstanding terrain-patch requests (not a global throughput promise).
The production terrain-analysis scheduler is **configurable**:
`server/max_concurrent_analyses=1` (default) serializes whole analysis jobs;
2–4 permit overlapping analysis jobs after a service restart. The HTTP
`/status` response reports both the configured worker count and per-analysis
Map request cap. At 4 active workers and 4 Map requests per worker there may
be as many as 16 outstanding HTTP calls, but this is not a Map global limit.
The independent redistribution and coupled-simulation queues remain unchanged.
A borrowed in-process Map source must be explicitly verified thread-safe
before raising analysis worker count.

Run against your development servers only. This workload creates **persistent
analyses**; a default 1/2/4-job × three-round test creates 21 new analyses.
There is no automatic deletion or server-side cancellation if a test fails.
Use a fresh small terrain domain first and inspect Map load, then consider
larger inputs. The example below uses the same measured 195,712-cell GLO-30
region as the single-job tests:

```bash
./tools/benchmark_map_jobs.py \
  --west 8.600 --south 50.650 --east 8.750 --north 50.800 \
  --resolution-m 30 --jobs 1,2,4 --rounds 3 \
  --poll-seconds 0.05 --source-patch-size 256 \
  > benchmark-map-jobs.json

jq '{created: .total_jobs_created, results: [.results[] | {
  jobs, wall_ms: .wall_ms.median,
  throughput_jobs_per_second: .throughput_jobs_per_second.median,
  relative_throughput_vs_single,
  job_latency_ms: .job_latency_ms.median,
  queue_wait_ms: .queue_wait_ms.median,
  terrain_fetch_ms: .stage_timings_ms.terrain_fetch.median,
  worker_ms: .stage_timings_ms.total_worker.median
}]}' benchmark-map-jobs.json
```

The report also records `server_settings.max_concurrent_analyses` and
`server_settings.map_requests_per_analysis` from Hydrology `/status` when
available, so outputs from differently configured runs can be compared
without guessing the active INI settings. For each job, `terrain_fetch_details`
contains the validated existing Map HTTP request timings and patch counters;
`results[].terrain_fetch_details` includes their per-level distributions.
`map_request_ms` is the **sum** of overlapping request lifetimes and must not
be treated as elapsed time. `map_request_wall_ms` spans first request issue
to last completion and is the relevant network interval; `terrain_fetch`
includes additional setup and raster assembly. These values distinguish
transport contention from D8 and hierarchy CPU time but do **not** measure
Map's internal CPU usage or globally outstanding Map requests. Missing
optional fields from older servers appear as `null`; mixed availability in
one measured level is rejected to prevent misleading medians.

To test whether Map request oversubscription limits four-worker throughput,
keep `[server] max_concurrent_analyses=4`, change only `[terrain]
max_concurrent_requests` from `4` to `2`, and **restart Hydrology**. Confirm
`/status` before each run, and repeat the same live benchmark with identical
bounds, budgets, number of rounds, and running Map instance. For a
948,600-cell job, one round of the 1/2/4-job comparison requires explicitly
setting `--max-cells 1000000 --max-batch-cells 5000000
--max-total-cells 8000000`; each run creates seven persisted jobs. Example
summary of each resulting report:

```bash
jq '{settings: .server_settings, results: [.results[] | {
  jobs, throughput: .throughput_jobs_per_second.median,
  fetch_ms: .stage_timings_ms.terrain_fetch.median,
  map_wall_ms: .terrain_fetch_details.map_request_wall_ms.median,
  map_request_sum_ms: .terrain_fetch_details.map_request_ms.median,
  copy_ms: .terrain_fetch_details.raster_copy_ms.median,
  hierarchy_ms: .stage_timings_ms.depression_hierarchy.median,
  peak_hydrology_kib: .rss_kib.hydrology.peak_kib.median,
  settled_hydrology_kib: .rss_kib.hydrology.settled_kib.median
}]}' benchmark-map-jobs-large.json
```

The default 4-request cap may outperform 2 even under high load; neither
configuration is preferable without comparing measurements. `settled_kib`
can remain elevated due to allocator arenas, memory caches, or live objects,
and should not be labelled a leak without allocation/lifetime evidence.

Newly submitted jobs now include `stage_timings_ms.queue_wait` in their
persistent metadata: the monotonic wall time between enqueueing in the
analysis thread pool and starting its worker. It is **excluded** from
`total_worker`. The multi-job report exposes `queue_wait_ms` as an individual
job measurement and a per-concurrency-level distribution; old servers that
do not report it yield `null`, not a fabricated zero. With
`max_concurrent_analyses=1`, queued waves should show much larger waits
than their actual worker times. With a higher worker cap, remeasure both
batch throughput and peak **Map/Hydrology RSS**. Do not assume more workers
always improves performance: it adds CPU competition, increases live DEM
memory and can multiply Map requests.

The benchmark stops each wave's wall timer when the **last** analysis reports
`completed`, then optionally observes delayed RSS, and then reads a stored
elevation raster window for every job. Those checks, and an optional
independent Map DEM-parity check when `--map-url` is
supplied, are deliberately excluded from the completion wall time; they
also cannot affect other jobs within that wave. All jobs must agree on
dimensions, dataset, basin count, conditioned cells and the sampled elevation
window's SHA-256. Full-grid DEM identity is **not** implied by a matching
sampled window. Different result values abort the benchmark.

`relative_throughput_vs_single` is the job count multiplied by the median
single-job wave wall time divided by that level's median wave wall time;
this is a *batch throughput comparison*, not a sustainable jobs-per-second
rate or a concurrency guarantee. The report retains all individual measured
wave and job observations, plus per-job stage times and measured peak Map
patch concurrency. Warmup waves are optional; they also create saved jobs but
are excluded from aggregate results. The order of concurrency levels rotates
to limit systematic warm-cache/order effects.

Resource limits are enforced **before** submitting any job:

- `--jobs` must be a unique comma-separated subset of 1..4 containing 1;
  `--rounds` is 1..5, `--warmup-rounds` is 0..2, and at most 40 new jobs
  are permitted per benchmark invocation.
- `--max-cells` limits each job (default 250,000); `--max-batch-cells`
  limits simultaneous requested cells (default 1,000,000); and
  `--max-total-cells` limits all persisted new cells (default 8,000,000).
- To test a larger region, raise those budgets **explicitly**, choosing fewer
  job levels or rounds if needed; the server's own `max_cells` still applies.
  `--job-timeout` applies separately to every submitted job.

For optional live Linux **process RSS** sampling, supply the actual process
IDs from your local Map and Hydrology services; e.g. append:

```bash
  --map-pid <MAP_SERVER_PID> --hydrology-pid <HYDROLOGY_SERVER_PID>
```

Only when PIDs are supplied, the tool periodically reads `/proc/<pid>/status`
`VmRSS` during each wave, reporting `initial_kib`, `peak_kib`, `last_kib`,
`peak_increase_kib`, and sample counts for each process. It also waits
`--rss-settle-seconds` (default **2.0**, valid **0..10**) **after** the timed
wave and takes a separate process reading: `settled_kib` and
`settled_change_kib` (relative to initial). Set the delay to 5 seconds for
comparisons with manual settled-RSS checks; zero means immediate post-wave
sampling. RSS data is provided for every wave and as per-process distributions
under `results[].rss_kib`; these are absent (`null`) without PIDs. The delay
and subsequent DEM-result verification are both excluded from
`completion_wall_ms` and throughput. For example, after a live run with PIDs:

```bash
jq '[.results[] | {jobs, hydrology_peak_kib: .rss_kib.hydrology.peak_kib.median,
  hydrology_after_delay_kib: .rss_kib.hydrology.settled_kib.median,
  map_peak_kib: .rss_kib.map.peak_kib.median}]' benchmark-map-jobs.json
```

Settled means **delayed sampled RSS**,
not proof that workers are all destroyed, all malloc arenas were reclaimed,
or no other process work occurred; the result is not a leak detector.
PID accessibility is
validated before any job is submitted; benchmark failure can still leave jobs
running. These samples are **not** peak total system RAM, include neither
unreported child processes nor Map's cache allocations that happened before
the wave, and can miss brief short-lived peaks. `--rss-poll-seconds` defaults
to 0.025 (valid 0.01..1 second). Use system monitoring for memory pressure,
CPU scheduling, disk I/O and other tenants while interpreting the results.

Run local, no-server protocol regression checks with:

```bash
python3 tests/test_benchmark_map_jobs.py -v
```

The opt-in `AOWIS_HYDROLOGY_BUILD_BENCHMARKS=ON` CTest suite registers the
same offline test. Ordinary Debug/Release builds and the 19-test service
suite remain unchanged; the tool does **not** need new C++ instrumentation.

## Verification and reference comparisons

The benchmark ensures **internal conservation**, not physical correctness.
Separate validation with *actual* GLO-30 data should cover:

1. Source coverage, georeferencing, vertical datum and Map raster delivery.
2. NoData holes, boundaries, internal pits, coastlines, and flat terrain.
3. D8 flow direction, accumulation and catchment boundaries compared against
   an independently configured GIS solver (e.g. GRASS GIS or WhiteboxTools),
   with identical resampling, conditioning, D8 tie rules and catchment limits.
4. Basin volume/level curves checked against direct integration from the
   original elevation raster; basin-merge topology checked on crafted fixtures.
5. Sensitivity to DEM resolution, vertical error, breaching, polygon area,
   runoff fraction, calibrated controls and travel-speed assumptions.
6. Independent HTTP/Map and in-process-service parity on identical terrain
   samples; verify source checksums and scenario provenance across restarts.
7. CPU/RAM and query latencies for district, city and metropolitan extents,
   including acquisition, persistence, repeated cache reads, and concurrent
   requests—not only this isolated core benchmark.

Differences against a reference implementation do not prove an error unless
algorithm, boundary, cell-area and DEM-processing policies are equivalent.
Measurements from real local terrain and independently verified results must
be recorded **separately** from synthetic timings. Document observed errors,
not just successful runs, before using the outputs for engineering decisions.

## Current limits

- Synthetic and real Map/Hydrology benchmarks exist, but current published live results cover up to 195,712 DEM cells; not yet representative metropolitan scale.
- In-memory core algorithms; the 10-million-cell cap is a safety budget, not
  a guarantee that a particular machine has enough RAM.
- No speedup assertion, throughput SLA, calibration result or metro-sized
  performance claim is implied by the presence of this benchmark.
- Peak memory and stages include allocations from GeographicLib and the C++
  runtime, but omit other services and server-process caches.
- The FNV elevation fingerprint verifies deterministic *input identity* in
  reports, not the integrity of externally delivered DEM files.


## Live Map → Hydrology HTTP benchmark

`tools/benchmark_map_http.py` is a separate **Python 3 standard-library-only**
client. It uses the *actual running* Hydrology service and optionally Map's
analysis-grade DEM endpoint. It never generates synthetic input and never
creates its own Copernicus cache or accesses Map's private file paths.

**Prerequisites:** Start Map (normally port 8122), then start Hydrology
(normally port 8123) with `terrain/mode=rest` and the correct `terrain/map_url`.
Map must have permission to fetch or already have source DEM data covering the
requested bounds. Choose a small WGS84 area first. Commands below use an
**illustrative** coordinate window—replace it with a known supported area.

```bash
# Keep live tests small initially (the client defaults to 250,000 cells max).
./tools/benchmark_map_http.py \
  --hydrology-url http://127.0.0.1:8123 \
  --map-url http://127.0.0.1:8122 \
  --west 8.700 --south 50.700 --east 8.720 --north 50.720 \
  --resolution-m 30 --repeats 5 \
  --source-patch-size 256 \
  > benchmark-map-http.json

# Use the saved job ID from the report to measure retrieval after restart,
# without submitting another analysis job or downloading the DEM again.
./tools/benchmark_map_http.py \
  --existing-job <job-uuid-from-json> \
  --map-url http://127.0.0.1:8122 \
  > benchmark-persisted-query.json

# Optional: measure logical on-disk output lengths on the Hydrology host.
./tools/benchmark_map_http.py \
  --existing-job <job-uuid-from-json> \
  --analysis-directory /path/to/hydrology/analyses \
  > benchmark-persisted-disk.json
```

For a Map installation using read-key authentication, supply
`--map-read-api-key` on the command line; it is sent **only to Map**, not to
Hydrology, and is excluded from reports. Shell process arguments and shell
history can still expose keys, so use appropriately protected test machines.
To benchmark only Hydrology queries without a direct Map comparison, supply
`--map-url ''`.

### Measured operations

1. `GET /status` validates the Hydrology endpoint.
2. In new-job mode, the client estimates the raster size using the **same**
   WGS84/resolution formula as the service, refuses a workload larger than
   `--max-cells` **before POST**, submits `POST /v1/jobs`, and polls its job
   until completed, failed, canceled, or timed out. Its `job_wait_s` includes
   terrain acquisition, D8, basin hierarchy and persistence, but it does **not**
   separately time those internal operations.
3. Reads the same bounded `GET /v1/jobs/{id}/raster/elevation` float32 window
   repeatedly, records all HTTP wall times and confirms byte-identical
   responses (content type and length must also match).
4. Reads the first bounded `/basins` JSON page repeatedly; records wall times
   and ensures content stability.
5. Optionally **reconstructs the original Map acquisition patches** used by
   Hydrology (`terrain/patch_size`, default 256), requests those same source
   windows from `/terrain/v1/analysis/grid`, and crops the returned float32
   samples to the saved Hydrology raster window. It does **not** independently
   resample the smaller comparison window: because Map selects source pixels
   using `floor()`, an insignificant floating-point change in a cell-centre
   coordinate can select an adjacent 1-arcsecond DEM pixel. It checks the
   resulting data against Hydrology's persisted original DEM, failing if any
   cell differs by more than `--elevation-tolerance-m` (default 0.0001 m),
   or if either response contains NaN/Inf. Reports SHA-256s, exact byte
   equality, the full Map bytes transferred, and number of source requests.
   **Set `--source-patch-size` to the patch size used by Hydrology when that
   job originally fetched its elevations**, including for existing jobs.
   This may transfer more Map data than the small Hydrology comparison window.
6. Optionally counts file lengths beneath an explicitly supplied local
   Hydrology analysis-output directory. This does not report allocated disk
   blocks or account for caches outside that job.

If parity fails, stderr now includes the analysis job UUID, maximum difference,
first differing cell coordinates, and the corresponding Map/Hydrology values.
Do not raise the elevation tolerance to make native-pixel mismatches pass.
First check `--source-patch-size`, server terrain source configuration, and
whether Map source data changed since the job was created. A persistent
mismatch with identical acquisition geometry is a meaningful discrepancy.

The script never deletes completed analyses, alters cache settings, performs
concurrent load, or starts or stops servers. The data set and conditioning
policy are included in the JSON report. The *existing-job* mode never posts
anything; use it after an actual server restart to test job and raster
persistence, because re-querying during the same process lifetime does not
prove restart recovery.

### Server-side stage timing (new analyses)

A completed analysis now includes a `stage_timings_ms` object in the normal
`GET /v1/jobs/{job-id}` response. It is also retained in the job's `job.json`
manifest and copied to `server_stage_timings_ms` in the live benchmark report.
This does **not** change the request, binary raster formats, or analysis files.
An older analysis created before this capability has an empty object; the
benchmark prints that empty object (or `null` for an older server without the
field). **Re-querying an existing job does not create historical timings.**

Example of the *shape* of the result (numbers are illustrative):

```json
"stage_timings_ms": {
  "queue_wait": 12.0,
  "terrain_fetch": 40.0,
  "terrain_validation": 0.5,
  "d8_analysis": 75.0,
  "raster_persistence": 20.0,
  "depression_hierarchy": 160.0,
  "basin_persistence": 5.0,
  "total_worker": 315.0
}
```

| Stage key | Start and end of measurement |
| --- | --- |
| `queue_wait` | Elapsed time between submitting a persisted job to the terrain-analysis thread pool and entering the worker; **excluded** from `total_worker` |
| `terrain_fetch` | Entire injected/Map terrain reader call, including HTTP, retries, assembling source tiles; excludes coverage validation |
| `terrain_validation` | Geometry/coverage validation before D8 computation |
| `d8_analysis` | `analyze()` including terrain conditioning, flow directions and upstream accumulation |
| `raster_persistence` | Writing and committing all seven analysis raster files |
| `depression_hierarchy` | `buildDepressionHierarchy()` on the original DEM |
| `basin_persistence` | Encoding and atomically writing `basins.aowbin` |
| `total_worker` | Elapsed time in `runJob()` until completion is about to be recorded (or until failure/cancellation) |

All figures are **monotonic wall-clock milliseconds**, not CPU usage. Individual
stages exclude most intermediate metadata-manifest writes, Qt bookkeeping and
scheduler overhead. Accordingly, `total_worker` is expected to be at least
the sum of the execution stages **excluding `queue_wait`** (within floating-point error); don't infer that
the remainder is idle CPU time. The final completion-manifest commit follows
the `total_worker` measurement. Failed or canceled jobs may expose only the
stages they reached plus `total_worker`.

The client-reported `job_wait_s` is different: it includes queuing and
asynchronous polling delay. In particular, the client observes states only at
`--poll-seconds` intervals. Comparing repeated actual-region runs at the same
resolution is more informative than using a synthetic hierarchy benchmark to
infer Map/network costs. Server timing does not show Map's internal stages or
identify whether Map downloaded new Copernicus tiles versus serving cache.

To benchmark the same area with the new fields, **restart the Hydrology server
with the updated executable**, then submit a *new* analysis:

```bash
./tools/benchmark_map_http.py \
  --west 8.650 --south 50.670 --east 8.750 --north 50.770 \
  --resolution-m 30 --poll-seconds 0.05 --repeats 5 \
  > benchmark-district-stages.json
```

Only after this separation should we prioritize optimizing Map acquisition,
D8, basin hierarchy, persistence or result queries. For a larger run, adjust
`--max-cells` consciously and monitor the server's resident memory separately.

**Important performance distinctions:** The measured full-job wall time
includes Map network/download and disk writes; synthetic C++ core time does
not. A first request can hit an already-populated Map cache. The Map sample
is intentionally taken **after** the job, so it measures a warm or previously
fetched region and cannot be called a cold-download benchmark. Polling
observes state transitions with sampling delay; it does not give exact
per-stage server CPU times. Client-side HTTP latency is not server-side
algorithm time. `--analysis-directory` measures **logical bytes**, not
physical block usage or process RSS. Run repeated jobs with explicitly
controlled Map cache states before drawing cold-versus-warm conclusions.

### Live client options and resource control

| Option | Default | Notes |
| --- | --- | --- |
| `--west/south/east/north` | required for new job | Valid WGS84 bounds; no automatic bounding box |
| `--resolution-m` | 30 | Same 30–5000 m limit as Hydrology |
| `--max-cells` | 250,000 | Explicit safety budget; raise deliberately for district/city/metropolitan experiments |
| `--window` | 64 | Hydrology elevation comparison window width/height, maximum 512 |
| `--source-patch-size` | 256 | Must match Hydrology `terrain/patch_size` at terrain acquisition, maximum 512; determines independent Map source-request geometry |
| `--basin-limit` | 25 | Bounded result page, maximum 100 |
| `--repeats` | 5 | Sequential HTTP samples, 1–100 |
| `--poll-seconds` | 0.25 | Polling interval, maximum 60 seconds |
| `--job-timeout` | 600 s | Client timeout; exceeding it does not cancel the server job |
| `--request-timeout` | 180 s | Per-HTTP-call timeout |
| `--existing-job` | unset | Queries UUID-addressed completed analysis, no new POST |
| `--map-url` | 127.0.0.1:8122 | Empty string disables independent DEM parity |
| `--elevation-tolerance-m` | 0.0001 | Absolute comparison tolerance, not DEM uncertainty |
| `--analysis-directory` | unset | Optional local analysis-output root; read-only |

The script exits nonzero with a human-readable error on HTTP failure,
incomplete source coverage, failed job, inconsistent raster samples, missing
persisted data, or elevation mismatch. On success it prints exactly one JSON
report to standard output. Redirect this to a file along with the synthetic
benchmark reports and system metadata. It does **not** claim Map results
are scientifically independent reference elevations: both Map and Hydrology
are reading the same source through different interfaces. This comparison
checks *data transport and raster alignment*, not hydrological correctness.

To exercise only the live-client protocol against a local synthetic HTTP
fixture (without Map/Qt servers):

```bash
python3 -m unittest discover -s tests -p test_benchmark_map_http.py -v
```

When `AOWIS_HYDROLOGY_BUILD_BENCHMARKS=ON` and Python 3 is available, the
same offline protocol test is registered under the `benchmark-smoke` CTest
label.

### Interpreting a synthetic pitted-terrain baseline

A high synthetic `basin_count` means the test created many tiny local
minima/merges. In this workload, depression-hierarchy construction often
dominates CPU time; benchmark it separately before attempting an algorithmic
optimization. Do not extrapolate its basin density to real GLO-30 terrain.
Compare RSS at individual stages, not just the total, and record whether the
run released its transient D8 result before building the hierarchy. When
comparing a live HTTP job with the synthetic benchmark, account for additional
resident source buffers, Qt allocations, Map cache, persistent files and
potential concurrent requests.


### Bounded concurrent Map acquisition (HTTP)

Hydrology now issues up to four concurrent Map analysis-grid REST GETs within
**one** terrain-analysis worker. The default `terrain/max_concurrent_requests=4`
is based on three local six-patch experiments, where four parallel Map reads
reduced probe wall time by approximately 1.88–2.12× compared with sequential
reads. This is a *probe result*, not a promise of the same improvement for a
full Hydrology worker or for other Map deployments.

Use an INI file with:

```ini
[terrain]
patch_size=256
max_concurrent_requests=4
request_timeout_ms=180000
```

The valid concurrency range is 1–4, checked by both the executable's INI
parser and the library API. `1` selects the previous sequential behavior and
is the recommended A/B comparison baseline. Each Hydrology worker maintains
its **own Qt network manager and queue**; it never shares replies or a manager
across threads. Requests preserve identical patch bounds, source sample
precision, WGS84 cell-centre handling and per-patch response checks. Replies
are placed by row/column rather than completion order. No unbounded per-patch
request array is allocated; at most four payloads can be in flight. Cancel
and error paths abort remaining replies without returning a partial DEM.
The shared, borrowed Map C++ object (`map_in_process`) remains sequential
regardless of this setting; concurrent calls into that object would require
explicit Map-side guarantees that are not assumed here.

Run repeated jobs with `max_concurrent_requests=1` and again with `4`, keeping
Map's cache and CPU load comparable, to evaluate **full-worker** speedup. The
same raster hashes, basin counts and conditioned-cell counts must match. In
addition inspect `server_terrain_fetch_details.peak_in_flight`,
`map_request_wall_ms`, and `map_request_ms` and the series' `overlap_ratio`
metric. Full-worker timing includes D8, basin hierarchy and persistence, so
its speedup will normally be smaller than the Map-only concurrency probe.
A large number of simultaneous Hydrology *jobs* can multiply outstanding Map
GETs (`jobs × max_concurrent_requests`); production scheduling must bound job
concurrency independently. This is not a new Map rate limit or a change to
Map's own cache policy.
