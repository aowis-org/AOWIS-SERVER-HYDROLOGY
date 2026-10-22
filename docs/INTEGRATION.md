# Hydrology integration architecture

AOWIS-SERVER-HYDROLOGY is the owner of terrain-flow analysis, depression storage, downstream tracing and rainfall-volume scenarios. Its computational engine must be reusable in both distributed and standalone builds. Map is the owner of DEM retrieval and caching; GUI is the owner of interaction and rendering. SWMM will be a separate component.

## Deployment modes

### Distributed deployment (HTTP)

```text
AOWIS-SERVER-GUI -- HTTP --> aowis-server-hydrology
                                    |
                                    +-- HTTP --> aowis-server-map
```

The GUI submits terrain-analysis and rainfall-scenario requests, polls or subscribes to progress, and fetches results. Hydrology requests analysis-grade DEM grids from Map, not quantized rendering tiles. Remote hydrology analysis jobs run independently of the GUI's map view and may continue after a client disconnects.

### Standalone deployment (linked libraries)

```text
aowis-server-gui (same process)
  |-- Hydrology application service (C++/Qt library)
  |       |-- pure C++ analysis core
  |       +-- terrain-source interface -- AOWIS::Map (C++ library)
  +-- AOWIS::Map (shared instance/source)
```

Standalone must not spawn Hydrology or Map HTTP daemons, create implicit localhost sockets, or implement an alternative hydrology algorithm. It invokes the same application-service operations in process. Map's existing configuration governs DEM availability, download policies and caches. A Map object supplied by the application is borrowed and must outlive the Hydrology service; ownership remains with the standalone application.

The existing `AOWIS::Hydrology` target exposes the pure C++ `analyze()` algorithm. `AOWIS::HydrologyService` now owns the **existing terrain-analysis job API**, including background execution, persistence, cancellation, binary result retrieval and an injectable terrain callback. HTTP `Server` delegates to this library. Rainfall scenarios and persisted static basin-overflow redistribution use that same application service; background redistribution tasks share the same service, with task status and cooperative cancellation. The legacy C++ redistribution method remains synchronous, but REST GET is cache-only. GUI wiring and time-dependent hydraulics remain future work. See [RUNOFF_REDISTRIBUTION.md](RUNOFF_REDISTRIBUTION.md), [REDISTRIBUTION_CACHE.md](REDISTRIBUTION_CACHE.md) and [ASYNCHRONOUS_REDISTRIBUTION.md](ASYNCHRONOUS_REDISTRIBUTION.md). Call `AnalysisService` directly instead of using HTTP server types in Standalone.
`ServiceConfig::max_concurrent_analyses` (default 1, permitted range 1..4)
controls the number of active terrain-analysis jobs in either integration;
other submissions remain queued. A value above one requires a reentrant
`TerrainReader`/borrowed Map source and additional per-job memory. It does not
change the separate, one-worker redistribution and coupled-simulation queues.

## Stable interface principles

1. One **Hydrology application service** owns validation, job lifecycle, analysis caching, progress, cancellation, result persistence and scenario execution. Neither the GUI nor the HTTP adapter contains hydrology algorithms.
2. One **terrain-source abstraction** delivers elevations with full georeferencing, NoData, vertical reference and dataset identity. Planned implementations: HTTP Map adapter, borrowed in-process Map adapter, and local GeoTIFF input for analysis and tests.
3. Both GUI-facing interfaces provide equivalent operations and semantics: create/reuse a terrain analysis, submit a rainfall scenario, inspect progress, cancel a job, query drainage paths/catchments and retrieve result windows. HTTP serializes service inputs/outputs; standalone calls the same service directly.
4. Analysis identifiers and scenario identifiers are separate. Terrain-derived flow/depression graphs can be reused for many rainfall polygons and depths.
5. Algorithms do not depend on Qt, HTTP or `AOWIS::Map`; the application-service layer may use Qt facilities for scheduling, notifications and persistence.
6. Processing/selection domains are independent: a rainfall polygon, the output region, and the actual terrain computation region are distinct. Incomplete upstream/downstream coverage must be reported.
7. Dataset access in Hydrology never creates a second Copernicus downloader. Acquisition, caching, supported DEM datasets and remote-fetch policy remain under Map's control.

## Target libraries and build behavior

The current target structure, with future growth planned for scenario operations:

| CMake target | Role | Linked by |
| --- | --- | --- |
| `AOWIS::Hydrology` (existing) | Pure C++ analysis algorithms | Hydrology service, core tests |
| `AOWIS::HydrologyService` (terrain jobs and rainfall scenarios implemented) | Terrain jobs, results, scheduling, injected terrain sources | HTTP adapter and future Standalone GUI |
| `aowis-server-hydrology` (thin adapter/CLI) | Qt HTTP routes, CLI and process lifecycle | Distributed server deployment |

Qt HttpServer is not required when linking the standalone application-service library. The standalone build already adds Map and GUI as CMake subdirectories. When Hydrology is included, Map must be added only once; the Hydrology build must reuse the parent's `AOWIS::Map` target. The optional existing local-Map build configuration recognizes a previously defined target. Standalone-specific wiring, submodule entries and GUI linking will be changed only in a separately authorized cycle.

Current terrain-job and rainfall-scenario service embedding:

```cmake
set(AOWIS_HYDROLOGY_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(AOWIS_HYDROLOGY_BUILD_SERVICE ON CACHE BOOL "" FORCE)
add_subdirectory(AOWIS-SERVER-HYDROLOGY)
target_link_libraries(aowis-server-gui PRIVATE AOWIS::HydrologyService)
```

Use `ServiceConfig::terrain_reader` for an in-process terrain source, passing terrain-grid geometry and the cancellation token. This callback runs in the service worker thread. Its captured objects must outlive the service.

## Terrain access contracts

The current REST implementation requests `GET /terrain/v1/analysis/grid` from Map, returning float32 elevations sampled from native source DEMs at specified raster cell centres. The **matching Map patch** implements both the HTTP endpoint and `Aowis::Map::TerrainData::analysisGrid()` for in-process callers. Apply that patch before expecting cross-server analyses to succeed. Use local/synthetic test readers for offline algorithm tests.

When Map returns partial or missing data, preserve NoData diagnostics; do not invent heights, silently change resolution, or treat rendering `.aowterrain` tiles as analysis truth.

### Offline DEM fixtures for tests and development

Hydrology retains `AOWIS::HydrologyRaster` (GDAL) as a separate development/test library, not a production terrain backend. The production Hydrology server always queries Map over HTTP; a standalone build borrows its already-initialized Map `TerrainData` object through `SourceConfig::local_map` or a supplied `TerrainReader` callback. The Hydrology server does not accept a local DEM path, create another Map instance, or maintain a separate Copernicus cache. Map now offers `TerrainData::analysisGrid()` and an equivalent `/terrain/v1/analysis/grid` endpoint in the accompanying Map patch. See [docs/GEOTIFF.md](GEOTIFF.md) for standalone GDAL test-library use.

## Consistency and integration tests

- Synthetic inputs produce identical flow graphs and volume balances in service-local and HTTP modes.
- A standalone GUI can query Hydrology and Map through library calls with no Hydrology or Map listener running.
- A distributed GUI can submit an identical job through HTTP and obtain equivalent results.
- Hydrology uses one in-process Map instance when provided; never adds Map twice in CMake or allocates its own source cache.
- Slow jobs report progress and cancel promptly in both modes.
- Terrain acquisition failure, NoData regions and insufficient upstream/downstream extent are observable to both clients.
- Repeated rainfall scenarios reuse terrain-analysis results without repeating preprocessing.

## Repository ownership

This integration cycle includes **authorized** updates to `AOWIS-SERVER-HYDROLOGY` and `AOWIS-SERVER-MAP` only, delivered in separate ZIP patches. `AOWIS-SERVER-GUI` and `AOWIS-SERVER-Standalone` remain unchanged; any future updates to those repositories require separate confirmation.

## Stage 2 geospatial interface

`Aowis::Hydrology::ElevationGrid` now contains a `RasterGeometry` with explicit `HorizontalCrs`, separate `VerticalDatum` metadata, origin at the outer NW edge and positive cell extents east/south. Geographic WGS84 sources remain native, so Map's planned source-grade GeoTIFF patches are still requested as EPSG:4326. The existing service/REST job request remains geographic-only; internal UTM grids are supported in `AOWIS::Hydrology` without forcing a Map or GUI change. See `docs/GEOMETRY.md` for data contracts.

A parent Standalone meta-build may define `GeographicLib::GeographicLib` once and share it between GUI and Hydrology. The Hydrology CMake project checks for this target before including its own submodule. Do not add the same GeographicLib source directory twice.

## D8 drainage queries (Stage 5 first increment)

The reusable service and the HTTP adapter provide matching `downstreamTrace`
and `channelNetwork` operations against persisted terrain analyses. Both
return WGS84 GeoJSON; internal flow indexes remain relative to the analysis
raster. The server obtains no new Map data for these queries. Raster
computation domain and requested downstream distance remain separate: a
trace ending at a domain boundary is explicitly reported as boundary-limited.
See [DRAINAGE_NETWORKS.md](DRAINAGE_NETWORKS.md).

## Static standing-water outputs (Stage 8)

After the rainfall redistribution cache is ready, a separate asynchronous
`startStaticInundation()` operation can reconstruct connected-basin water
levels and persist a derived f32 raster. The C++ library and REST transport
expose the same `staticInundationStatus()` and bounded
`staticInundationDepthWindow()` operations. Client GET requests never launch
calculation. On incomplete sibling merges the status is
`unresolved_partial_merge` and no depth is available; callers **must not**
substitute a zero-depth map. The complete sequence, binary framing and
source-invalidation rules are documented in
[INUNDATION_SERVICE.md](INUNDATION_SERVICE.md).
