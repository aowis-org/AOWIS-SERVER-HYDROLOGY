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
| `aowis-server-hydrology` | CLI configuration and HTTP route presentation | Qt HttpServer, service |

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
- Analysis queue concurrency is configured independently from asynchronous
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
