# AOWIS Hydrology Server

AOWIS Hydrology provides terrain-based drainage analysis, depression storage,
rainfall-runoff scenarios, static inundation estimates, and experimental
reservoir-network routing. It runs as a standalone HTTP service or as reusable
C++ libraries inside a host application. The computation engine does not depend
on the GUI, Map's HTTP adapter, or SWMM.

**Division of responsibilities:** [AOWIS Map](docs/INTEGRATION.md) supplies
analysis-grade elevations and controls DEM acquisition/caching; Hydrology
performs calculations and persists results; a GUI or other client supplies
scenarios, displays outcomes, and decides the analysis domain. Hydrology does
not download Copernicus datasets itself, operate a second terrain cache, or
run SWMM.

## What the software provides

| Capability | Available interface | What the result means |
| --- | --- | --- |
| Terrain conditioning, D8 direction and contributing area | C++ core, service, HTTP | Topographic routing and potential fill diagnostics; original DEM is retained |
| Catchments, downstream paths, channels, watershed outlines and affected-network queries | C++ core, service, HTTP | D8-derived drainage connectivity within the requested domain |
| Original-DEM depression hierarchy, saddles, basin capacities and storage curves | C++ core, service, HTTP | Topographic storage without rainfall or flow timing |
| Polygon rainfall scenarios and per-cell effective runoff | C++ core, service, HTTP | Event-total runoff volumes under an assumed runoff fraction |
| Static fill–spill–merge-style redistribution | C++ core, service, HTTP | Volume allocation, basin storage and domain outflow, **not a dynamic flood model** |
| Static ponded-depth raster | C++ core, service, HTTP | Conditional standing-water estimate; some partial merges cannot produce a valid raster |
| Spill-contact geometry, alternative sibling allocations, calibrated hydraulic ratings | C++ core | Explicit diagnostics and optional physical laws; no control geometry is inferred from terrain alone |
| Coupled reservoir routing and external outlets | C++ core | Experimental time-stepped transfer between explicitly characterized reservoirs |
| Coupled-simulation jobs and scenario-derived forcing | C++ core, service, HTTP | Experimental simulation with user-provided curves, controls and time profiles |
| GeoTIFF input for development and tests | Optional GDAL library | Local raster fixture reader; **not** an alternate production DEM server |

Hydrology distinguishes **three different kinds of output** that must not be
confused: potential terrain-fill depth, event-total runoff redistribution, and
time-dependent reservoir routing. They use related but different assumptions.
Neither mass conservation nor a plausible inundation map establishes
scientifically calibrated flood prediction. Important limitations include
30 m source-DEM detail, cropped domain edges, buildings/vegetation in GLO-30,
unknown real-world inlet/culvert geometry, simplified runoff fractions, and
uncalibrated hydraulic controls. See [Product scope and open questions](docs/ROADMAP.md).

### Technical documentation

**Terrain and drainage:** [Grid geometry and vertical reference](docs/GEOMETRY.md),
[DEM conditioning](docs/CONDITIONING.md), [Drainage networks](docs/DRAINAGE_NETWORKS.md),
[Depression hierarchy](docs/DEPRESSION_HIERARCHY.md),
[GeoTIFF fixture reader](docs/GEOTIFF.md).

**Rainfall and static water:** [Rainfall scenarios](docs/RAINFALL_SCENARIOS.md),
[Runoff redistribution](docs/RUNOFF_REDISTRIBUTION.md),
[Asynchronous redistribution](docs/ASYNCHRONOUS_REDISTRIBUTION.md),
[Cache format and lifecycle](docs/REDISTRIBUTION_CACHE.md),
[Static inundation model](docs/STATIC_INUNDATION.md),
[Inundation service](docs/INUNDATION_SERVICE.md),
[Sibling allocation sensitivity](docs/SIBLING_ALLOCATION_SENSITIVITY.md),
[Capacity-weighted sibling allocation](docs/CAPACITY_WEIGHTED_SIBLING_ALLOCATION.md).

**Hydraulic routing:** [Spill geometry](docs/SPILL_GEOMETRY.md),
[Free-overflow controls](docs/SPILL_HYDRAULICS.md),
[Calibrated/submerged controls](docs/SUBMERGED_SPILL_CONTROLS.md),
[Coupled reservoir routing](docs/COUPLED_RESERVOIR_ROUTING.md),
[numerical verification and reference cases](docs/NUMERICAL_VERIFICATION.md),
[External outlets](docs/BOUNDARY_OUTLETS.md),
[Timed runoff](docs/TEMPORAL_RUNOFF_FORCING.md),
[D8 travel-time approximation](docs/TRAVEL_TIME_RUNOFF.md),
[Coupled-simulation service](docs/COUPLED_SIMULATION_SERVICE.md),
[Saved-scenario forcing](docs/SCENARIO_COUPLED_FORCING.md).

**Integration and performance:** [Architecture and source contracts](docs/INTEGRATION.md),
[Benchmark methodology and commands](docs/BENCHMARKING.md),
[Measured performance and engineering conclusions](docs/BENCHMARK_FINDINGS.md).

**Release verification:** [Automated contracts and product acceptance](docs/VERIFICATION.md).

## Building and testing

Requires CMake 3.21+, a C++20 compiler and GeographicLib. Qt 6 Core and
Network are required for the reusable application service; the HTTP executable
uses the pinned, vendored cpp-httplib v0.58.0 header instead of Qt HttpServer.
Ninja is used by the Linux convenience scripts. GDAL is optional for the
separate GeoTIFF fixture library. Initialize cpp-httplib once if the vendored
source is not yet present (a generated handoff ZIP includes it after setup):

```bash
./tools/setup_cpp_httplib.sh
```

The setup script checks out the fixed upstream release and copies its header
and license into `external/cpp-httplib/`; CMake never downloads dependencies.
Include those generated source files in Git so ordinary clones and future ZIPs
are self-contained.

```bash
# With existing dependencies:
./compile_linux.sh --no-run       # Build the server without starting it
./compile_linux_tests.sh --both   # Build and test Debug and Release
./compile_linux.sh                # Build and start the HTTP service

# Debian-family dependency installation, when needed:
./compile_linux.sh --install-dependencies --no-run
```

The tests default to the service/REST build and cover pure-core algorithms,
job persistence, terrain transport, configuration, and HTTP-facing service
logic. The `aowis-hydrology-http-contract` test runs the real server against
an isolated deterministic Map mock, checking HTTP status/error contracts,
binary rasters, independent numerical runs and restart persistence.
Assertions remain enabled for Release tests. Core-only testing is
available with `./compile_linux_tests.sh --both --core-only`. A normal CMake
build also works:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure -L hydrology
```

Initialize the GeographicLib submodule after cloning:

```bash
git submodule update --init --recursive
```

`./tools/zip_hydrology.sh` packages the current worktree **including all
initialized Git submodules recursively** (such as `external/GeographicLib`),
plus non-ignored uncommitted and untracked source files within them. Git metadata
and ignored build products are excluded. Packaging fails with an initialization
command if any tracked submodule is missing; a handoff ZIP must never silently
omit required build dependencies. The resulting ZIP contains ordinary source
files and can be reviewed or built without separately cloning submodules.

The vendored GeographicLib is always built as a **static** library
(`GeographicLib::GeographicLib_STATIC`), so executables never depend on a
shared library inside the build tree, and its `BUILD_SHARED_LIBS` default does
not leak into a parent build. A parent build may supply GeographicLib first;
Hydrology then links `GeographicLib::GeographicLib_STATIC` when that target
exists, otherwise `GeographicLib::GeographicLib`. GeographicLib's own tool
tests are skipped by a generated `CTestCustom.cmake`, so a plain `ctest`
runs only Hydrology tests. `tools/setup_geographiclib.sh` is available for
initial submodule setup. No dependency is downloaded implicitly by CMake.

A top-level build installs the server executable with
`cmake --install build --prefix /opt/aowis` (option `AOWIS_HYDROLOGY_INSTALL`,
default ON only when Hydrology is the top-level project).

### Linked-library and standalone use

- `AOWIS::Hydrology`: Qt-independent D8, basins, rainfall and reservoir
  algorithms. Disable both service and server for core-only builds.
- `AOWIS::HydrologyService`: Qt Core/Network application service with jobs,
  persistence, queries and injectable terrain readers; no HTTP dependency.
- `AOWIS::HydrologyRaster`: optional GDAL-backed GeoTIFF development reader.
- `aowis-server-hydrology`: thin cpp-httplib HTTP/CLI adapter over the service.

For a parent application that already has an initialized Map library:

```cmake
set(AOWIS_HYDROLOGY_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(AOWIS_HYDROLOGY_BUILD_SERVICE ON CACHE BOOL "" FORCE)
add_subdirectory(AOWIS-SERVER-HYDROLOGY)
target_link_libraries(aowis-server-gui PRIVATE AOWIS::HydrologyService)
```

Inject a `ServiceConfig::terrain_reader` callback or configure the optional
`AOWIS_HYDROLOGY_USE_LOCAL_MAP=ON` adapter to borrow the caller's Map instance.
The caller owns that instance for the entire service lifetime; a shared Map
instance must be thread-safe before increasing terrain analysis workers.
The HTTP executable always obtains terrain over Map REST. See
[Integration](docs/INTEGRATION.md) for exact ownership and API behavior.

## Configuration and operation

The server uses a **user-local Qt INI file**. On Linux the default is usually
`~/.local/share/aowis-server-hydrology/hydrology.ini`, or the corresponding
`$XDG_DATA_HOME/aowis-server-hydrology/hydrology.ini`; other systems use their
Qt application-data location. Its default result directory is the sibling
`analyses/` folder. Startup does not create the INI implicitly.

```bash
./build-linux/aowis-server-hydrology --write-default-config
vim ~/.local/share/aowis-server-hydrology/hydrology.ini
./build-linux/aowis-server-hydrology --check-config
./compile_linux.sh
```

`--write-default-config` without an argument uses the platform default path;
existing files are protected unless `--overwrite` is supplied. Use
`--print-default-config` to print a template or `--config PATH` to select
another file. `--check-config` checks syntax/values, **not** network
connectivity. INI comments use `;`, not `#`.

Typical settings (illustrative, not a recommended RAM budget for all machines):

```ini
[server]
listen_address=127.0.0.1
port=8123
output_directory=
max_cells=10000000
max_concurrent_analyses=1
max_outstanding_analyses=8
max_active_terrain_cells=0
http_workers=8
http_max_queued_requests=32
http_read_timeout_s=15
http_write_timeout_s=30
http_keep_alive_timeout_s=5
max_concurrent_heavy_queries=2
hierarchy_cache_basins=4000000
api_key=
allow_unauthenticated_remote=false

[terrain]
dataset=copernicus-glo30
mode=rest
map_url=http://127.0.0.1:8122/
map_read_api_key=
request_timeout_ms=180000
patch_size=256
max_concurrent_requests=4
```

The inbound HTTP server uses 1–64 independent request workers (default 8)
and a bounded waiting queue of 1–1024 connections (default 32). The INI
also exposes read, write and keep-alive timeouts in seconds. Individual JSON
request bodies are capped at 256 KiB by the transport *before* the service
handles them; the API retains its stricter endpoint-specific validation.
Large binary raster responses are streamed in bounded chunks without an extra
full-size `std::string` copy. Analysis workers and Map HTTP patch concurrency
remain independently controlled. Embedded builds with
`AOWIS_HYDROLOGY_BUILD_SERVER=OFF` do not require cpp-httplib.

`max_cells` limits the size of each terrain analysis. The independent
`max_active_terrain_cells` guard controls the **sum of cells in dispatched
terrain workers** (zero inherits `max_cells`; explicit values 4..400 million).
An analysis larger than this running-cell budget is rejected before creating
output. Accepted jobs that would exceed the budget remain in a FIFO queue
until earlier workers finish. This is a **cell-count concurrency limit**,
not a bound on RSS: algorithms have variable memory per cell, and separate
rainfall, catchment, redistribution and coupled tasks may overlap. Measure
actual process RSS under representative concurrent workloads before sizing a
production deployment. /status reports `terrain_jobs.active_cells` and
`terrain_jobs.active_cell_limit` alongside queue counts.

`max_concurrent_analyses` accepts 1–4 (default 1) and controls whole-job
parallelism. `max_outstanding_analyses` accepts 1–64 (default 8) and must
be at least `max_concurrent_analyses`. It caps the **combined queued and
running** terrain jobs; completed, failed and canceled jobs do not occupy
admission slots. Submissions beyond the cap return HTTP `429` with a JSON
error and do not create job directories. Canceled queued jobs free their slot
immediately and never perform terrain fetches. The service uses its own
cancelable queue rather than accumulating arbitrary pending thread-pool tasks.
`GET /status` exposes `terrain_jobs` with queued, running, outstanding and
limit counts; the same metrics are available through the C++ service.
`max_concurrent_requests` accepts 1–4 (default 4) and controls
**Map HTTP patches per active analysis**, not a global Map limit. Four workers
with four requests per worker may have 16 Map requests outstanding. The
borrowed in-process Map interface remains sequential. Increasing workers can
multiply peak RAM; benchmark representative requests before doing so.
Configuration changes require a server restart.

**Stored-result queries read persisted rasters through memory maps** instead
of loading them: channel networks, windowed and polygon-affected networks
allocate memory proportional to the selected/affected cells only (a 40M-cell
network query fell from about 960 MB of heap to 2 MB), and basin storage and
curves read only the basin's cells plus a one-bit-per-cell visited set.
Mapped pages are shared, reclaimable page cache. Queries that still scan or
materialize whole grids (channel networks, affected networks, uncached basin
hierarchies, synchronous rainfall scenarios) are limited to
`max_concurrent_heavy_queries` (1–16, default 2) at once; further requests
receive HTTP `429` ("Heavy query capacity busy"). Asynchronous
rainfall tasks wait for the same slots. Decoded basin hierarchies with their
UUID index are cached up to `hierarchy_cache_basins` basins in total (0
disables the cache), so basin paging and lookups no longer decode the archive
or compute one UUID per basin on every request.

**Authentication:** with `api_key` set (16–256 printable ASCII characters),
every route except `GET /status` requires `X-API-Key: <key>` or
`Authorization: Bearer <key>`; otherwise `401`. A non-loopback
`listen_address` is refused without `api_key` unless
`allow_unauthenticated_remote=true` is set explicitly (for example behind an
authenticating reverse proxy). `map_url` may carry a path prefix
(`http://proxy/map` and `http://proxy/map/` are equivalent).

**Startup and shutdown:** restored analyses are checked structurally (files,
lengths, hierarchy checksum) during startup; the SHA-256 content check runs in
the background after the listener is up. Until it finishes, the job reports
`"artifact_verification": "pending"` and its data endpoints answer `503`
("Analysis integrity verification in progress"). Corrupted analyses then turn
`failed`. SIGINT/SIGTERM stop the server in order (HTTP, jobs, workers) and
remove the transient catchment spool; spools of killed processes are removed
by the next start.

**HTTP status codes** for service errors follow one table
(`src/server/http_status.h`): `429` capacity/busy/queue full (including the
redistribution, inundation and coupled-simulation queues, formerly `409`),
`503` verification pending, `409` not completed / not ready / no active task,
`404` unknown entity, `400` rejected input, `500` storage corruption or I/O.

Map must support `GET /terrain/v1/analysis/grid`, returning row-major,
north-to-south float32 little-endian source heights sampled at requested cell
centres (`application/vnd.aowis.dem-f32le`). Invalid cells are NaN; incomplete
domains are rejected by the analysis. Quantized map-rendering tiles are **not**
valid hydrological input. The REST request uses geographic WGS84 extents,
with an approximately metric target resolution. See [Geometry](docs/GEOMETRY.md)
and [Integration](docs/INTEGRATION.md).

## HTTP workflow

Create and monitor a terrain job:

```bash
curl -sS -X POST http://127.0.0.1:8123/v1/jobs \
  -H 'Content-Type: application/json' \
  -d '{"west":8.600,"south":50.650,"east":8.750,"north":50.800,"resolution_m":30,"fill_depressions":true}'
curl -sS http://127.0.0.1:8123/status
curl -sS http://127.0.0.1:8123/v1/jobs
curl -sS http://127.0.0.1:8123/v1/jobs/JOB_UUID
```

Job submission returns a UUID. Statuses include queued, terrain fetch,
analysis, basin processing, saving, completion, cancellation and failure.
Results persist across restarts; interrupted jobs are marked failed.
`DELETE /v1/jobs/{id}` requests cancellation, **not removal** of saved files.
A queued cancellation is persisted immediately and remains canceled on restart;
a running job is requested to stop cooperatively. Finished job archives and
scenarios are **retained indefinitely by default**, even after cancellation.
For explicitly requested offline retention cleanup, stop the Hydrology server
and use `tools/prune_hydrology_analyses.py --output-directory PATH` to preview
terminal-job archives older than 30 days (adjust with `--older-than-days`).
Only `--delete --confirm-server-stopped` actually removes eligible UUID job
folders **including all derived scenarios and simulations**. Nonterminal jobs,
unknown directories and malformed manifests are preserved. There is no automatic
age-based deletion or public purge operation: result deletion must not race
running analysis or reads from GUI clients.
Optional locally constrained breaching uses explicit maximum cut depth, path
length, excavation volume and candidate budgets; fill is the default.

Useful GET/POST endpoints after job completion:

| Workflow | Endpoint (relative to `/v1/jobs/{analysis-id}`) |
| --- | --- |
| Original/conditioned elevation, flow, contributing area and fill diagnostics | `GET /raster/{layer}?x=0&y=0&width=256&height=256` |
| Basin list/detail and water-level storage | `GET /basins`, `/basins/{id}`, `/basins/{id}/storage?level_m=...`, `/basins/{id}/curve?samples=9` |
| Catchment and drainage queries | `GET /catchment?x=...&y=...`, `/trace?x=...&y=...&distance_km=20`, `/network?min_upstream_area_km2=2`, `/watershed?x=...&y=...` |
| Non-blocking full-grid catchments | `POST /catchment-tasks` with `{"x": 0, "y": 0}`, `GET/DELETE /catchment-tasks/{task-id}`, `GET /catchment-tasks/{task-id}/mask` |
| Geographically selected channels | `POST /affected-network` |
| Scenario creation and retrieval | `POST /rainfall-scenarios` (legacy synchronous), `GET /rainfall-scenarios`, `GET /rainfall-scenarios/{id}`, `GET /rainfall-scenarios/{id}/runoff?...` |
| Non-blocking scenario calculation | `POST /rainfall-scenario-tasks`, `GET /rainfall-scenario-tasks/{task-id}`, `DELETE /rainfall-scenario-tasks/{task-id}` |
| Static redistribution | `POST /rainfall-scenarios/{id}/redistribution`, `GET .../redistribution/status`, `GET .../redistribution?offset=0&limit=50`, `DELETE .../redistribution` |
| Static inundation | `POST /rainfall-scenarios/{id}/inundation`, `GET .../inundation/status`, `GET .../inundation/depth?...`, `DELETE .../inundation` |
| Coupled simulation | `POST /coupled-simulations`, `GET /coupled-simulations/{id}`, `GET /coupled-simulations/{id}/snapshots?...`, `DELETE /coupled-simulations/{id}` |

Catchment requests traverse the immutable memory-mapped downstream raster;
only the requested outlet's contributing-cell count is read, so each request
avoids materializing two complete 64-bit flow-graph arrays. One-byte-per-cell
output and the upstream traversal queue remain proportional to the domain and
catchment. The asynchronous API is preferable for large full-grid results.

`server/max_coupled_dem_cells` separately controls DEM-dependent coupled
simulations (10 million by default, up to 100 million with sufficient memory).
Purely explicit reservoir simulations do not load an elevation raster. Both
terrain and coupled cell ceilings are advertised by `GET /status`.

Most large raster responses are **headerless binary arrays**, window-limited
to 512 × 512 cells. Interpret their sample type, units and north-to-south row
order according to the relevant feature document and the saved job's geometry.
Raster window retrieval does not recompute terrain analysis. In particular,
redistribution and inundation GET operations are **cache-only**; submit the
corresponding POST first and inspect status before reading a result.

Rainfall scenarios accept WGS84 Polygon/MultiPolygon geometry, event rainfall
`depth_mm`, `duration_seconds` and `effective_runoff_fraction`. Static
redistribution reports retained basin volumes and separate direct/basin-boundary
outflows with mass-balance residual. **Only root basin storage** belongs in the
global retained-volume sum. Static inundation may return
`unresolved_partial_merge` with **no** depth raster; never substitute zero depth
for an unavailable estimate.

Coupled-simulation jobs require explicit reservoir stage–storage data and
calibrated connections/outlets. A saved scenario can supply event-total runoff
only with a caller-supplied time profile; constant-velocity D8 delays are
optional assumptions, not inferred calibrated hydrographs. See the feature
documents for exact request objects and model limitations.

## Performance and validation

Reproducible tests live in `tools/benchmark_hydrology.cpp` and
`tools/benchmark_map_*.py`; they are **opt-in**, with bounded work and JSON
reports. Start with [Benchmarking](docs/BENCHMARKING.md) for setup, workload
and result interpretation. [Benchmark findings](docs/BENCHMARK_FINDINGS.md)
records the measured synthetic and real-terrain cases, verified data parity,
speedups, scaling/memory observations and unresolved questions. Benchmark
results are **engineering evidence**, not flood-model validation. They also do
not establish a universally optimal worker/request setting.

## Repository tools

```bash
./tools/zip_hydrology.sh   # Build ../hydrology.zip for repository handoff
./compile_linux_tests.sh --both
```

The repository's `tools/apply_patch.sh` accepts only
`aowis-server-hydrology-*.zip` patches, with repository-relative paths and
fresh extraction timestamps. This is a developer workflow, not an aspect of
the Hydrology data model. The project archive excludes Git internals,
ignored build products and its submodule source tree.

### Durable primary analysis results

Primary terrain rasters and basin hierarchy archives are committed as immutable
files before a job becomes `completed`. Completed jobs include an artifact
integrity inventory (SHA-256 and exact byte sizes), verified upon server
restart. Missing or corrupt primary results are reported as failed rather than
served as completed analyses. Older archives without these hashes remain
readable after structural and length checks. See `docs/VERIFICATION.md`.
