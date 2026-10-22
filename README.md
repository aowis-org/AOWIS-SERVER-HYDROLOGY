# AOWIS-SERVER-HYDROLOGY

AOWIS terrain-based hydrology analysis service. It is independent of the GUI and of the planned **AOWIS-SERVER-SWMM** service. The same analysis capabilities are intended to run as a dedicated HTTP service or inside the [AOWIS-SERVER-Standalone](https://github.com/aowis-org/aowis-server-standalone) application as linked C++ libraries, without running local HTTP servers.

See [Integration architecture](docs/INTEGRATION.md) for GUI–Hydrology–Map contracts, [GeoTIFF development reader](docs/GEOTIFF.md) for test fixtures and direct library usage, and [Implementation roadmap](docs/ROADMAP.md) for stages and verification criteria.

## Current capabilities

The first implementation provides:

- A reusable, Qt-independent **C++20** hydrology core (`AOWIS::Hydrology`), with GeographicLib geodesy and D8 analysis entry points usable by linked applications.
- Regional elevation-grid analysis using **Priority-Flood depression filling**, flat-area routing without artificial elevation ramps, **D8 flow direction**, and **flow accumulation** in upstream cells and square metres.
- **Physical depression-fill diagnostics** including per-cell potential fill depth and area-weighted total potential fill volume; the original DEM remains unchanged. See [DEM conditioning](docs/CONDITIONING.md).
- **Depression hierarchy and basin storage (core/service/REST):** 8-connected raster-minimum and saddle topology, nested parent/child basins, physical spill capacities and bounded water-level/volume queries derived from the original terrain. See [Depression hierarchy](docs/DEPRESSION_HIERARCHY.md). Basin artifacts now persist with analysis jobs, with UUID-addressed queries and sampled curves.
- **Rainfall scenarios (core/service/REST):** independent UUID-addressed scenarios reusing a completed terrain analysis, WGS84 GeoJSON polygons/multipolygons, exact planar clipped-cell coverage and polygon union including holes, explicit total rainfall depth/duration/runoff fraction, mass-balanced gross/effective volumes, persistent row-major runoff rasters, and scenario retrieval after restart. See [Rainfall scenarios](docs/RAINFALL_SCENARIOS.md).
- **Static Fill–Spill–Merge-style runoff redistribution (core/service/REST):** original-DEM dry D8 routing, closed-basin storage, sibling/parent spill transfers, domain outflow and explicit mass balance, queried for saved rainfall scenarios. Results are cached per scenario in a versioned, integrity-checked binary archive and reused across pagination and restarts. A bounded asynchronous worker provides nonblocking REST submission, status, cancellation and cache-only retrieval. This remains a volume-only screening approximation. An opt-in pure C++ diagnostic compares two alternative sibling-spill allocations and reports leaf basins whose predicted storage depends on ordering. A separate, optional remaining-capacity-weighted allocation removes child index precedence for exploratory results; neither method changes the cached default or supplies a hydraulic flow law. See [Runoff redistribution](docs/RUNOFF_REDISTRIBUTION.md), [Sibling allocation sensitivity](docs/SIBLING_ALLOCATION_SENSITIVITY.md), [Capacity-weighted sibling allocation](docs/CAPACITY_WEIGHTED_SIBLING_ALLOCATION.md), [Result caching](docs/REDISTRIBUTION_CACHE.md) and [Asynchronous jobs](docs/ASYNCHRONOUS_REDISTRIBUTION.md).
- **Spill-saddle contact geometry (core only):** identify original-DEM contact locations between lower basin components and their merging saddle, distinguish cardinal raster faces from corner-only D8 neighbours, and collapse nested equal-elevation merge events so they are not mistaken for separate physical spillways. Diagnostic only: neither contact-length totals nor corner adjacency is a hydraulic flow law. See [Spill geometry](docs/SPILL_GEOMETRY.md).
- **Explicit hydraulic spill-control foundation (core only):** independently specified rectangular broad-crested controls can now be associated with validated two-component saddle contacts to calculate **free-overflow** signed discharge and one mass-conserving, crest-limited volume-transfer step. Hydraulic width and discharge factor must come from external measurements/calibration; diagonal-only and multiway saddles, submerged/backwater flow, coupled hydrologic timing and server integration are **not** supported by this primitive. Existing cached static redistribution and inundation results are unchanged. See [Spill hydraulics](docs/SPILL_HYDRAULICS.md).
- **Conservative static standing-water reconstruction (core/service/REST):** invert physical basin storage volumes to water levels and cell-centre depth rasters. At unformed saddles, reconstruct independent child ponds with an explicit `allocation_assumed` warning; at formed saddles, reconstruct the connected merged pond. Reject inconsistent allocations. A separately scheduled task persists source-validated depth rasters with bounded, checksum-verified REST windows and status reporting. See [Static inundation model](docs/STATIC_INUNDATION.md) and [Inundation service API](docs/INUNDATION_SERVICE.md).
- **Catchment delineation** upstream of a chosen terrain cell.
- **Downstream tracing and D8 channel-network extraction:** reusable flow-graph queries, length/slope metrics, tributary junctions and Strahler stream order. Includes **watershed GeoJSON polygons**, geographically selected downstream-affected channels and bounded window extraction. See [Drainage networks](docs/DRAINAGE_NETWORKS.md).
- Optional **constrained local breaching** of short DEM obstructions, with explicit excavation and path limits, physical cut-depth diagnostics and separate unmodified DEM elevations. Disabled by default.
- A transport-independent **`AOWIS::HydrologyService`** library (`Qt Core` + `Qt Network`, no Qt HttpServer). It owns the job queue, persistence, cancellation, bounded raster retrieval and catchment queries. An optional C++ terrain-reader callback allows in-process input and deterministic tests.
- A thin **Qt 6 REST adapter** (`aowis-server-hydrology`) exposing that service's operations over HTTP, with no separate job/analysis implementation.
- Acquisition of source-precision floating-point elevation windows through **AOWIS-SERVER-MAP**, via HTTP in a distributed deployment and the existing Map C++ `TerrainData` instance when embedded. The Qt-independent, read-only `AOWIS::HydrologyRaster` GeoTIFF reader remains available separately for development fixtures and controlled reference comparisons; it is not a production server terrain provider.

The job service has a reusable in-process interface for the same operations served over HTTP. Rainfall scenarios and the initial static redistribution model are implemented. Time-dependent SWMM simulation, generally applicable flood-depth maps and complete standalone GUI wiring remain separate roadmap work.

### Interpretation and limitations

Copernicus GLO-30 is a digital **surface** model. Its nominal resolution is insufficient for reliable street-level gutters, culverts, individual drainage inlets, building-scale routing, or flood depths. Analysis results are **topographic potential drainage**, not calibrated stormwater discharge or flood predictions.

D8 accumulation counts **all contributing cells** (including the cell itself); it is not a volume or flow rate. The analysis currently uses a geographic WGS84 raster, with latitude-adjusted physical slopes and areas. Analysis bounds must enclose **the relevant upstream watershed**; results are necessarily truncated at any clipped boundary. The present implementation is in-memory and targets regional GLO-30 data, not nationwide metre-resolution DEMs.

The input terrain and conditioned elevations are kept separately. The filled physical surface is flat at a depression's spill level—**no artificial epsilon height increment is added**. A separate flat-routing graph provides D8 connectivity. Use `fill_depressions=false` to preserve closed basins as real sinks. The potential fill-depth raster is available in either mode. The diagnostic fill volume is not a rainfall-storage or flood estimate. See [DEM conditioning](docs/CONDITIONING.md) and [Depression hierarchy](docs/DEPRESSION_HIERARCHY.md). Connected basin storage is exposed through the reusable service and REST API, but remains distinct from flood simulation.

## Geometry and GeographicLib

Hydrology uses [GeographicLib](https://github.com/geographiclib/geographiclib) **2.7**, the version present in the current GUI repository snapshot. It must be registered in the **Hydrology** repository as its own Git submodule. Run once from the Hydrology checkout:

```bash
./tools/setup_geographiclib.sh
# Creates/stages .gitmodules and the external/GeographicLib gitlink.
# Commit those two entries in this repository before distributing source through Git.
```

Existing clones then use `git submodule update --init --recursive`. The submodule sources are intentionally **not included in Hydrology patch ZIPs**. If a parent Standalone/GUI CMake build already defines `GeographicLib::GeographicLib`, Hydrology reuses that target; it will not compile a second copy. `AOWIS_HYDROLOGY_GEOGRAPHICLIB_SOURCE` can override the source location for local testing.

The C++ core now accepts **north-up geographic WGS84 grids (EPSG:4326)** and **north-up WGS84 UTM grids (EPSG:32601–32660 / EPSG:32701–32760)**. For Copernicus, the REST job geometry remains EPSG:4326, with GLO-30's EGM2008 elevation datum carried as distinct metadata. The core does not resample elevations when converting coordinates, and it does not conflate ellipsoidal heights with EGM2008 elevations. See [geometry documentation](docs/GEOMETRY.md).

## Build

Requires CMake >= 3.21, C++20 and Qt 6 **Core**, **Network** and **HttpServer** to build the REST executable; the reusable service requires only Core and Network. Ninja is required by the Linux helper scripts.

The recommended Linux workflows are:

```bash
./compile_linux.sh --install-dependencies --no-run  # Debian-family: install requirements and compile
./compile_linux.sh --no-run                  # Build the REST executable without starting it
./compile_linux.sh -- --print-default-config # Build and run a server CLI command
./compile_linux.sh                           # Build and start the server

./compile_linux_tests.sh                     # Build server + run tests (RelWithDebInfo)
./compile_linux_tests.sh --both              # Independently test Debug and Release
./compile_linux_tests.sh --both --core-only  # Test pure C++ core without Qt installed
```

`compile_linux.sh` uses `build-linux` and defaults to `RelWithDebInfo` (override with `AOWIS_BUILD_TYPE`). `compile_linux_tests.sh` uses separate `build-linux-tests`, `build-linux-tests-debug`, and `build-linux-tests-release` directories. Test assertions remain enabled in Release, and the script fails if no Hydrology tests are registered. The normal test script builds the Qt executable as well as the core to catch compilation issues; `--core-only` deliberately skips that integration check.

If you have already installed Qt and dependencies through another method, do not use `--install-dependencies`. Current Debian-family package names are `build-essential cmake ninja-build qt6-base-dev qt6-httpserver-dev libgdal-dev`.

Alternatively, use CMake directly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure -L hydrology
```

The executable is `build/aowis-server-hydrology`. To build and test the pure C++ hydrology core without Qt:

```bash
cmake -S . -B build-core -DAOWIS_HYDROLOGY_BUILD_SERVER=OFF
cmake --build build-core --parallel
ctest --test-dir build-core --output-on-failure -L hydrology
```

### Terrain input and ownership

For a normal HTTP deployment, configure Hydrology to use the **Map** service:

```ini
[terrain]
dataset=copernicus-glo30
mode=rest
map_url=http://127.0.0.1:8122/
# Optional if the existing Map server requires its configured read key:
map_read_api_key=
request_timeout_ms=180000
patch_size=256
```

Hydrology no longer has a `terrain/local_dem_directory` or a production
`mode=geotiff`: the **Map server** owns terrain acquisition and cached files.
The Hydrology executable also does not instantiate an independent Map service.
The in-process/standalone application must supply its **already initialized**
`Aowis::Map::TerrainData` instance via `SourceConfig::local_map` (when built
with `AOWIS_HYDROLOGY_USE_LOCAL_MAP=ON`) or an injected `TerrainReader`.

The optional `AOWIS::HydrologyRaster` library remains a Qt-independent GDAL
GeoTIFF reader for unit tests and development tools. To exclude it completely,
set `-DAOWIS_HYDROLOGY_WITH_GDAL=OFF`. It is **not linked into the normal
Hydrology service or server**. See [docs/GEOTIFF.md](docs/GEOTIFF.md).

To build the optional Map C++ terrain adapter against the same terrain cache and download policy as AOWIS-SERVER-MAP:

```bash
cmake -S . -B build-local \
  -DAOWIS_HYDROLOGY_USE_LOCAL_MAP=ON \
  -DAOWIS_HYDROLOGY_MAP_SOURCE=/path/to/AOWIS-SERVER-MAP
cmake --build build-local --parallel
```

This build option requires a version of AOWIS-SERVER-MAP that includes the **analysis-grade terrain grid API**, `TerrainData::analysisGrid`. Using the shared library does not create a second DEM provider or copy its cache. If a parent CMake build has already defined `AOWIS::Map`, Hydrology reuses that target instead of adding Map a second time. Otherwise the repository path above provides it.

To embed the **analysis and job service libraries** without compiling an HTTP server, use this parent-CMake configuration:

```cmake
set(AOWIS_HYDROLOGY_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(AOWIS_HYDROLOGY_BUILD_SERVICE ON CACHE BOOL "" FORCE)
add_subdirectory(AOWIS-SERVER-HYDROLOGY)
target_link_libraries(aowis-server-gui PRIVATE AOWIS::HydrologyService)
```

`AOWIS::HydrologyService` exports the public API in `include/aowis/hydrology/service.h`: `initialize`, `submit`, `job`, `jobs`, `cancel`, `rasterWindow`, `catchment`, `downstreamTrace`, `channelNetwork`, and rainfall scenario creation/retrieval. Supply a `ServiceConfig::terrain_reader` function for an in-process terrain source, or let the existing adapter request DEM patches from Map. The caller owns any external Map instance and must keep it alive until the service is destroyed. This makes the job functionality reusable in-process; Standalone's own CMake and GUI integration still require separate authorization.

To build only the Qt-independent algorithm library, disable **both** `AOWIS_HYDROLOGY_BUILD_SERVER` and `AOWIS_HYDROLOGY_BUILD_SERVICE`; link `AOWIS::Hydrology`. See [Integration architecture](docs/INTEGRATION.md) for both modes.

## Configuration

Configuration is Qt **INI** format. The application does not create or alter a file on startup.

```bash
./build/aowis-server-hydrology --print-default-config
./build/aowis-server-hydrology --write-default-config ./hydrology.ini
./build/aowis-server-hydrology --config ./hydrology.ini --check-config
./build/aowis-server-hydrology --config ./hydrology.ini
```

`--write-default-config` refuses to replace an existing file unless `--overwrite` is specified. Without `--config`, the service checks its default data-location configuration path, `aowis-server-hydrology/hydrology.ini`, and uses documented defaults if none exists. No configuration is silently generated. `--check-config` validates settings; it is not a test of map-server connectivity.

Relevant INI options:

| Section/key | Purpose |
| --- | --- |
| `server/listen_address` | Bind address; defaults to loopback `127.0.0.1` |
| `server/port` | REST port; defaults to `8123` |
| `server/output_directory` | Persistent job/result storage; blank means platform default |
| `server/max_cells` | Maximum cells per requested analysis (default 10 million) |
| `terrain/dataset` | Dataset identifier used by Map (initially `copernicus-glo30`) |
| `terrain/mode` | `rest` (the executable); standalone selects a C++ Map source through the library API |
| `terrain/map_url` | Map server base URL (REST mode; defaults to `http://127.0.0.1:8122/`) |
| `terrain/map_read_api_key` | Optional existing Map read key when configured by Map |
| `terrain/request_timeout_ms` | Timeout per Map REST patch |
| `terrain/patch_size` | Maximum patch dimension, 1–512 cells |

`terrain/map_url` accepts HTTP or HTTPS URLs. Hydrology obtains elevation samples through Map, which remains responsible for acquisition and caching. No Copernicus downloader or independent Map configuration is started by Hydrology.

### Required Map interface

Hydrology intentionally delegates elevation retrieval to Map, including its cache, enabled/disabled status and allowed external downloads. The existing `.aowterrain` 65×65 quantized rendering tiles are **not** suitable as the canonical hydrology input.

The REST mode uses the following **Map endpoint**, added in the matching Map patch:

`GET /terrain/v1/analysis/grid?dataset=copernicus-glo30&west=...&south=...&east=...&north=...&columns=256&rows=256`

It returns raw IEEE754 **float32 little-endian**, row-major, north-to-south elevations, one sample per cell centre, with `Content-Type: application/vnd.aowis.dem-f32le`. Invalid/missing elevations are NaN. The caller requests at most 512×512 cells, and all adjacent requests use an identical overall grid geometry, avoiding per-render-tile boundaries. Map provides this contract in its HTTP route and its equivalent in-process `TerrainData::analysisGrid` call. The matching Map patch must be applied for either mode. Native GeoTIFF source pixel values are sampled at the requested cell centres without bilinear interpolation. Request grid alignment matters: Hydrology currently constructs an approximately 30 m WGS84 analysis grid and Map samples the containing native pixel at each cell centre. This does not imply exact 1-arcsecond alignment. The service rejects incomplete input rather than treating missing DEM cells as outlets.

## Depression storage API

Each completed terrain analysis contains a checksum-validated, versioned `basins.aowbin`
archive derived from the **original DEM**. Jobs also expose `basin_count`.
The service computes these results once as part of each new job; previous
pre-release analyses without the artifact need to be rerun. Basin entity IDs
are deterministic **UUIDs within their analysis job**, not internal array
indices. Pagination bounds the HTTP response; no extra Copernicus downloads
are triggered by basin queries.

```http
GET /v1/jobs/{analysis-uuid}/basins?offset=0&limit=50
GET /v1/jobs/{analysis-uuid}/basins/{basin-uuid}
GET /v1/jobs/{analysis-uuid}/basins/{basin-uuid}/storage?level_m=12.0
GET /v1/jobs/{analysis-uuid}/basins/{basin-uuid}/curve?samples=9
```

The matching `AOWIS::HydrologyService` methods are `basins()`, `basin()`,
`basinStorage()`, and `basinStorageCurve()`. Pagination limits are 1–100,
curve samples 2–16. A basin response includes UUID-linked parent/children,
minimum cell coordinates, formation/spill elevations, capacity at spill,
area and cell count, and whether spill reaches the analysis boundary.
Unresolved spill values are JSON `null`. Storage curves return topographic
water volume and inundated cell area at evenly spaced levels between basin
formation and spill. A query may fail when the connected pond exceeds its
2-million-cell per-sample traversal budget; clients should then use spill
capacity from the basin summary rather than interpreting missing results
as zero. **These values are static topographic estimates, not simulated storm flooding.**

## Rainfall scenario API

Completed terrain analyses can be reused for independent, UUID-addressed
uniform-rainfall scenarios, with no repeated Map DEM fetching or D8 solving:

```http
POST /v1/jobs/{analysis-uuid}/rainfall-scenarios
GET /v1/jobs/{analysis-uuid}/rainfall-scenarios
GET /v1/jobs/{analysis-uuid}/rainfall-scenarios/{scenario-uuid}
GET /v1/jobs/{analysis-uuid}/rainfall-scenarios/{scenario-uuid}/runoff?x=0&y=0&width=256&height=256
```

The JSON POST accepts a WGS84 GeoJSON `geometry` (Polygon or MultiPolygon),
`depth_mm`, `duration_seconds`, `effective_runoff_fraction`, and an optional
`name`. It reports total rainfall volume, effective runoff volume and the
unrouted per-cell runoff raster. The `/runoff` endpoint returns raw little-
endian float64 **m³ per DEM cell**. Static storage and out-of-domain overflow **are available through the separate
redistribution endpoint**, but runoff rasters still do **not** represent flooded
areas or water heights. See [docs/RAINFALL_SCENARIOS.md](docs/RAINFALL_SCENARIOS.md).

## Rainfall redistribution and basin overflow (Stage 8)

A saved rainfall scenario can now be evaluated against the **original DEM's**
closed depressions and their saddles. The model routes each cell's effective
runoff along unfilled D8 drainage, retains volumes in topographic basins,
allows water to pass between merged depressions after a spill threshold, and
reports how much water leaves the analysis domain.

```http
POST /v1/jobs/{analysis-uuid}/rainfall-scenarios/{scenario-uuid}/redistribution
GET /v1/jobs/{analysis-uuid}/rainfall-scenarios/{scenario-uuid}/redistribution/status
GET /v1/jobs/{analysis-uuid}/rainfall-scenarios/{scenario-uuid}/redistribution?offset=0&limit=50
DELETE /v1/jobs/{analysis-uuid}/rainfall-scenarios/{scenario-uuid}/redistribution
```

The JSON response contains `generated_runoff_m3`, `basin_storage_m3`,
`direct_boundary_outflow_m3`, `basin_boundary_outflow_m3`,
`total_boundary_outflow_m3`, and `mass_balance_error_m3`, plus paginated
UUID-addressed basin states. **Only root basin storage is included in the
summary:** internal basin results include their children and must not be
summed together. This is a **static volume-only screening model**. It does
not compute flood timing, velocity, detailed inundation extent or depth,
urban drainage, or discharge outside the analyzed DEM. **HTTP POST** starts a
bounded background calculation, and **GET status** reports readiness. **GET
results never starts computation**; it reads the persisted per-scenario cache
or returns HTTP 409 until ready. A missing, stale or damaged derived cache is
recomputed atomically upon a new POST, while later pages and post-restart reads
reuse the valid cache. The legacy C++ method remains synchronous for existing
in-process callers. Incomplete DEM coverage is rejected.

## Static standing-water depth API

After completing the **redistribution** job, submit separate static inundation
reconstruction and inspect its physical status:

```http
POST   /v1/jobs/{analysis-uuid}/rainfall-scenarios/{scenario-uuid}/inundation
GET    /v1/jobs/{analysis-uuid}/rainfall-scenarios/{scenario-uuid}/inundation/status
GET    /v1/jobs/{analysis-uuid}/rainfall-scenarios/{scenario-uuid}/inundation/depth?x=0&y=0&width=256&height=256
DELETE /v1/jobs/{analysis-uuid}/rainfall-scenarios/{scenario-uuid}/inundation
```

`state=completed` and `cache_ready=true` do **not** necessarily mean depth
is available. Inspect `inundation_status` / `depth_available`: an
`unresolved_partial_merge` is a completed model result with **no raster**.
For pre-merge child ponds, `allocation_assumed: true` indicates that the depth
raster follows the deterministic redistribution allocation rather than a
uniquely validated hydraulic equilibrium. Raster GET is cache-only and returns little-endian float32 depths in
metres (cell centres, original DEM). This is *standing water in depressions*,
not a prediction of moving floodwater. See [Inundation service and binary
windows](docs/INUNDATION_SERVICE.md) for sequencing, limits, example payloads
and interpretation; see [Physical model](docs/STATIC_INUNDATION.md).

Read **[Asynchronous redistribution jobs](docs/ASYNCHRONOUS_REDISTRIBUTION.md)**
for the HTTP workflow, state machine and cancellation contract.
Read **[Runoff redistribution and basin overflow](docs/RUNOFF_REDISTRIBUTION.md)**
for the physical assumptions, water-balance equations, worked example,
query lifecycle, C++ usage and known accuracy limits. See also
**[Result caching](docs/REDISTRIBUTION_CACHE.md)** for persistence, recovery,
format and operational verification. For its runoff input
see [Rainfall scenarios](docs/RAINFALL_SCENARIOS.md), and for physical basin
capacities see [Depression hierarchy](docs/DEPRESSION_HIERARCHY.md).

## REST API

The service executes at most one analysis job concurrently to limit peak DEM working memory. The HTTP API is a thin adapter over the same Hydrology job service callable by standalone applications directly, without a localhost HTTP round-trip.

Create an analysis job (WGS84 bounds; `resolution_m` is approximate, from 30 m to 5 km):

```bash
curl -X POST http://127.0.0.1:8123/v1/jobs \
  -H 'Content-Type: application/json' \
  -d '{"west":8.0,"south":50.0,"east":8.15,"north":50.12,"resolution_m":30,"fill_depressions":true}'
```

For conservative local obstruction removal, optionally include
`"breach_depressions":true`, `"maximum_breach_cut_depth_m":3`,
`"maximum_breach_path_cells":8`, `"maximum_breach_excavation_m3":100000`
and `"maximum_breach_candidates":2000` in the JSON request. The default
remains **Fill without breaching**. Breaching may also be combined with
`"fill_depressions":false` to preserve unbreached closed sinks. See
[conditioning details](docs/CONDITIONING.md) for assumptions and limitations.

The server returns `202 Accepted` with a UUID job ID and the exact grid geometry. Jobs progress through `queued`, `fetching-terrain`, `analyzing`, `saving`, and `completed` (or `failed` / `canceled`). The service writes a manifest per job and marks interrupted, unfinished jobs as failed on restart.

```bash
curl http://127.0.0.1:8123/status
curl http://127.0.0.1:8123/v1/jobs
curl http://127.0.0.1:8123/v1/jobs/JOB_ID
curl -X DELETE http://127.0.0.1:8123/v1/jobs/JOB_ID  # Request cancellation
```

Completed jobs support bounded raster-window retrieval:

```bash
curl 'http://127.0.0.1:8123/v1/jobs/JOB_ID/raster/upstream-area?x=0&y=0&width=256&height=256' \
  --output area.f64
curl 'http://127.0.0.1:8123/v1/jobs/JOB_ID/catchment?x=100&y=100' \
  --output catchment.u8
```

For a **point-based** downstream trace (distance measured along D8 links) or
an indicative channel network:

```bash
curl 'http://127.0.0.1:8123/v1/jobs/JOB_ID/trace?x=100&y=100&distance_km=20' \
  --output downstream.geojson
curl 'http://127.0.0.1:8123/v1/jobs/JOB_ID/network?min_upstream_area_km2=2' \
  --output drainage.geojson
```

Both return GeoJSON, including stopping reasons and topology properties.
Channel selection is bounded to 25,000 cells and 2,000 segments per request;
raise the threshold for larger domains. A trace ending at the raster boundary
indicates **incomplete terrain coverage**, not a verified hydrological outlet.
See [Drainage networks](docs/DRAINAGE_NETWORKS.md) for units and limitations.

Available raster layers: `elevation`, `conditioned`, `potential-fill-depth`, and `breach-cut-depth` (`f32le`, metres), `upstream-area` (`f64le`, m²), `upstream-count` (`u64le`, cells), and `downstream` (`i64le`, linear downstream index, `-1` for none). Job metadata includes `potential_fill_cells`, `potential_fill_volume_m3`, and `maximum_potential_fill_m` as original-DEM fill diagnostics, plus `accepted_breach_paths`, `rejected_breach_candidates`, `breached_cells`, `breach_excavation_m3`, and `maximum_breach_cut_m` for optional local breaching. These are physical terrain-conditioning values, not rainfall or flooding predictions. `catchment` is a full-grid, row-major `u8` mask (1 = upstream of selected cell including itself, 0 = other). Raster windows are at most 512×512 cells; `x=0,y=0` is the northwest corner. Binary payloads have no header; use job metadata for dimensions/georeferencing. A canceled job is retained in metadata; `DELETE` does not erase stored analysis results.

## Repository tools

The script is **`tools/zip_hydrology.sh`**, not `zip_hydrology.zip`. The `.zip` suffix in the originally uploaded repository was a mistaken filename, not an archive or an API.

```bash
# Run inside AOWIS-SERVER-HYDROLOGY:
./tools/zip_hydrology.sh
# Generates ../hydrology.zip with current tracked and untracked non-ignored files,
# including new/uncommitted work, but without .git or ignored build outputs.
```

Apply Hydrology-only patches using their exact filenames:

```bash
./tools/apply_patch.sh ~/Downloads/aowis-server-hydrology-stage8-redistribution.zip
./tools/zip_hydrology.sh
./compile_linux_tests.sh --both
```

The patch validator restricts archives to the `aowis-server-hydrology-*.zip`
filename prefix, rejects unsafe archive entries, and gives extracted files
fresh timestamps so CMake/Ninja rebuild them. Project ZIP archives exclude
Git internals and ignored build directories. The GeographicLib dependency is
tracked as a Git submodule; when cloning this repository run
`git submodule update --init --recursive`.

## Next work

The implementation stages and outstanding work are documented in
[docs/ROADMAP.md](docs/ROADMAP.md). Watershed polygon boundaries, polygon-to-downstream-network selection and
bounded network windows are implemented. Next are spatial indexes, production
regional DEM comparisons, and domain-expansion/accuracy investigations. The depression-hierarchy and rainfall-volume stages are implemented, and the
static runoff redistribution and persistent per-scenario result caching are available. The present downstream trace is
**point-based** and always stops at the analysis domain boundary, a preserved
sink, or a configured resource/distance limit. It does not simulate rainfall
travel time or discharge.
