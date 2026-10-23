# Hydrology service architecture and integration contracts

Hydrology owns terrain-flow analysis, depression-storage topology, scenario
calculations and derived result persistence. Map owns DEM acquisition and
caching; GUI owns interaction and rendering. Standalone embeds the same C++
service. This division keeps the scientific calculations independent of the
transport used to reach them.

## Deployment models

### Separate services (HTTP)

```text
GUI or client -> Hydrology HTTP :8123 -> Map HTTP :8122 -> Map-owned DEM data
                   | 
                   +-> Hydrology-owned analysis/scenario result archives
```

The client creates a terrain job with geographic bounds and target resolution,
then polls its UUID and retrieves cached results. Hydrology obtains
analysis-grade DEM patches from Map; no proxy through the GUI or rendering-tile
conversion is involved. Jobs may finish after the requesting client disconnects.

### In-process / standalone

```text
Host application
  +-- single Map instance (host-owned DEM policy and cache)
  +-- Hydrology application service (C++/Qt, jobs/results/persistence)
        +-- pure C++ hydrology algorithms
        +-- borrowed Map terrain adapter or injected TerrainReader
```

The host initializes Map and keeps it alive until the Hydrology service stops.
A borrowed Map object is **not** owned or destroyed by Hydrology. Avoid
running Map/Hydrology HTTP daemons inside Standalone. The algorithms and
application-service semantics are shared with the HTTP deployment; only the
terrain adapter and client transport change.

## Library targets

| Target | Responsibilities | Dependencies |
| --- | --- | --- |
| `AOWIS::Hydrology` | Geometry, conditioning/D8, basins, rainfall, redistribution, inundation, hydraulic routing | C++20, GeographicLib; no Qt |
| `AOWIS::HydrologyService` | Job lifecycle, async workers, cancellation, archive and cache reads, terrain-source callbacks | Qt Core and Network, core |
| `AOWIS::HydrologyRaster` (optional) | Read-only GeoTIFF development/test input | GDAL, core |
| `aowis-server-hydrology` | CLI configuration and HTTP route presentation | cpp-httplib, service |

Set `AOWIS_HYDROLOGY_BUILD_SERVER=OFF` and
`AOWIS_HYDROLOGY_BUILD_SERVICE=ON` when embedding the job service. A parent
build can supply an existing `AOWIS::Map` and
`GeographicLib::GeographicLib` target; the local Map adapter does not add
either target twice. Its `SourceConfig::local_map` implementation is enabled
with `AOWIS_HYDROLOGY_USE_LOCAL_MAP=ON`, or the caller can inject
`ServiceConfig::terrain_reader`. This callback executes in analysis worker
threads and must remain reentrant if `max_concurrent_analyses` exceeds one.

The HTTP adapter delegates to `AnalysisService`, rather than implementing a
second hydrology engine. Results and errors should remain consistent across
C++ and HTTP integrations. See the [README](../README.md) for build examples
and the specialist documentation for operation-specific payloads.

## DEM source and coordinate contract

The source interface delivers a georeferenced `ElevationGrid`, including
horizontal CRS, cell-centre sampling, NoData/validity, vertical reference and
dataset identity. Internal core grids support north-up geographic WGS84 and
WGS84 UTM rasters. HTTP job bounds are geographic WGS84; Map supplies native
source pixels at each requested cell centre and the job describes its full
grid geometry. Internal distance/area calculations use GeographicLib rather
than assuming degrees are metres. See [Geometry](GEOMETRY.md).

The Map HTTP contract is:

```text
GET /terrain/v1/analysis/grid?dataset=copernicus-glo30&west=...&south=...&east=...&north=...&columns=256&rows=256
Content-Type: application/vnd.aowis.dem-f32le
Body: row-major, north-to-south IEEE754 little-endian float32 elevations
```

Missing elevations are NaN. Patches must refer to consistent cell centres
on **one** requested raster grid, not independently rounded rendering tiles.
The corresponding Map C++ path is `Aowis::Map::TerrainData::analysisGrid()`.
The source's dataset and vertical datum (EGM2008 for the used Copernicus
GLO-30 terrain) remain explicitly identified. Terrain input is never silently
filled where a source patch is incomplete. The production HTTP executable
uses Map REST; optional GDAL GeoTIFF readers are for testing/development.

## Jobs, persistence and concurrency

- The analysis service owns UUID jobs, bounded requests, stage status,
  cancellation, manifests, raster and basin archives, scenario caches and
  checksum-based recovery/read validation.
- Catchment masks have a bounded asynchronous task API (`POST /v1/jobs/{id}/catchment-tasks`,
  `GET/DELETE /v1/jobs/{id}/catchment-tasks/{task-id}`, and `GET .../{task-id}/mask`).
  One full-grid calculation runs at a time; four may be queued/running; the four
  newest terminal tasks remain readable while the service is running. Masks
  are disk-spooled, not kept in the job map, and are deleted on server shutdown.
  Canceled queued tasks release capacity immediately. Full-grid catchment
  D8 graph traversal is serialized across both APIs; a second synchronous
  catchment request receives HTTP 429 rather than allocating another graph. The legacy synchronous
  `GET /catchment` remains compatible for existing clients; asynchronous
  requests are recommended for large grids. Catchment status and masks are not
  persisted across restarts.
- Rainfall scenario creation also has an opt-in async task API with at most
  two calculation workers and eight queued/running tasks; legacy synchronous
  scenario creation remains supported. Scenario-count admission is atomic for
  concurrent library callers. Task status is transient, persisted scenario
  archives are not.
- Active terrain-worker cells are bounded independently by
  `max_active_terrain_cells` (0 inherits `max_cells`, or explicit 4..400000000).
  The service checks individual job feasibility before creating a job folder
  and schedules admitted requests in FIFO order without exceeding the
  aggregate running-cell budget. This limit is not a peak-RSS guarantee:
  separate rainfall/catchment/redistribution/coupled worker pools may overlap.
- Terrain workers (`max_concurrent_analyses`, 1–4) are separate from admission
  (`max_outstanding_analyses`, 1–64, >= worker count). Admission counts queued
  plus running terrain jobs and in-progress submission reservations. The
  service reserves space before creating output paths, and concurrent API
  callers cannot overfill the queue. The returned error begins with
  `Terrain analysis capacity reached`; HTTP responds with JSON and 429.
- A service-managed pending queue holds jobs not yet given a worker. Canceling
  a queued job marks it `canceled`, persists its manifest, frees admission
  immediately and prevents terrain acquisition. Canceling running jobs signals
  cooperative cancellation. Completed/failed/canceled results do not occupy
  admission slots, and saved artifacts are retained by default; DELETE of a
  job UUID requests cancellation, not artifact deletion. An opt-in,
  dry-run-first offline retention tool (`tools/prune_hydrology_analyses.py`)
  can delete terminal analysis folders older than a chosen threshold only
  after the server has been stopped. This also removes dependent scenario
  artifacts; no automatic retention deletion occurs.
- `AnalysisService::terrainJobCapacity()` and the `terrain_jobs` object in
  `/status` expose `queued`, `running`, `outstanding`, `limit`, `active_cells`
  and `active_cell_limit` for operations
  and benchmarks. Analysis admission is separate from asynchronous
  redistribution, inundation and coupled-simulation task scheduling.
- HTTP Map retrieval supports 1–4 parallel patch calls per active analysis;
  the in-process borrowed Map path is sequential. There is no service-wide
  Map request semaphore in the current implementation.
- Some results are explicitly unavailable even after their task completes:
  unresolved partial basin merges can prohibit an inundation depth raster.
  Client applications must propagate that status, not display a zero raster.
- Saved rainfall scenarios specify total event volumes; time-dependent
  simulations require independent time profiles, reservoir storage curves,
  calibrated hydraulic controls and explicit interpretation of direct
  domain-boundary runoff.

## Integration verification

Check identical source-raster patches, cell-centre alignment, basin UUID and
count identity, numerical conservation, bounded raster formats and query
responses across library and HTTP calls. Ensure interrupted work is
recoverable or visibly failed, corrupt caches are not accepted, and Map's
source policy is not bypassed. Large-region and concurrent-client testing
must account for **both** Hydrology worker memory and Map process/cache
memory. The [benchmark guide](BENCHMARKING.md) describes the repeatable
validation tools and [findings](BENCHMARK_FINDINGS.md) record observed limits.

GUI and Standalone integration remains a distinct repository-level activity;
this module documents its interfaces but does not claim end-user GUI wiring
exists merely because these libraries are linkable.

### HTTP transport and worker ownership

The standalone executable uses the pinned cpp-httplib HTTP/1.1 adapter (not
Qt HttpServer). The `AOWIS::HydrologyService` target remains an independent
Qt Core/Network C++ library for embedded AOWIS. No local HTTP listener is
required when linking the library into Standalone or GUI.

The HTTP listener runs on its own thread, accepts connections into a bounded
cpp-httplib worker queue and invokes `AnalysisService` methods concurrently.
Asynchronous terrain, rainfall, catchment and simulation calculations use
service-owned scheduling rather than occupying request workers. Legacy
synchronous catchment/rainfall endpoints are retained for API compatibility,
so callers should prefer the corresponding task endpoints for expensive work.
Outbound Map REST requests continue to use Qt Network inside Hydrology's
terrain workers. Callers using borrowed in-process Map instances remain
responsible for that instance's thread-safety and lifetime.

The server enforces an overall 256 KiB inbound body limit; endpoint-specific
smaller limits remain in effect. Large binary results use a streaming HTTP
content provider; their *generation* can still require bounded raster buffers,
which is addressed separately by the resource/large-raster improvements.
