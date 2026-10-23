# Static inundation: service integration and raster delivery

## What this service delivers

After the runoff-redistribution task completes, Hydrology can reconstruct **standing water retained in terrain depressions** from the assigned basin volumes. Completed physical-model results include:

- `available`, `allocation_assumed: false`: reconstructed active connected ponds without an unresolved pre-merge sibling split.
- `available`, `allocation_assumed: true`: separate child-pond levels reconstructed **conditional on the deterministic runoff allocation** in a not-yet-connected merge. This is a useful screening layer, **not an independently determined hydraulic split**. The `surfaces` list may contain child basin UUIDs rather than only root UUIDs.
- `unresolved_partial_merge`: allocated volumes could not be consistently mapped to either separate children or a formed merged pond. **No depth raster is published.** This is a model availability outcome, not proof of zero flooding.

The task may also be `not_started`, `queued`, `running`, `cancel_requested`, `canceled`, or `failed`. These describe the **computation lifecycle**, not the hydrological model outcome. Do not interpret `not_started` or `unresolved_partial_merge` as zero inundation.

**This is not a flood forecast.** It excludes flowing runoff, flood timing, infiltration kinetics, storm drains, culverts and channel hydraulics. Even a numerically valid depth raster is a simplified terrain-screening result, especially with Copernicus GLO-30.

The reconstruction depends on the immutable *original* DEM, saved basin hierarchy, and **already completed** redistribution cache. It never uses the filled/breached routing elevations as the ponding floor. An original DEM/source change invalidates the derived cache.

## Operations

Base path: `/v1/jobs/{job_uuid}/rainfall-scenarios/{scenario_uuid}/inundation`

| Operation | HTTP | Expected result |
| --- | --- | --- |
| Submit/reuse | `POST {base}` | `202 Accepted` while scheduled; `200 OK` if a valid inundation cache exists |
| Poll | `GET {base}/status` | Task lifecycle, physical model outcome, and reconstructed basin surfaces |
| Read depth | `GET {base}/depth?x=0&y=0&width=256&height=256` | `200 OK` with little-endian float32 cell-centre depths; `409 Conflict` if incomplete or physically unavailable |
| Cancel | `DELETE {base}` | `202 Accepted` after cancellation requested; `409 Conflict` if no task is active |

**Ordering matters:** first submit and finish **rainfall redistribution** (its own POST/status API); then submit inundation. Inundation submission returns `409 Conflict` if the redistribution cache is missing, stale, or inconsistent with scenario totals. Reading depth or polling status **never launches computation**.

### HTTP example

```bash
BASE="http://127.0.0.1:8123/v1/jobs/JOB_UUID/rainfall-scenarios/SCENARIO_UUID"

# 1. First obtain the required volume allocation.
curl -i -X POST "$BASE/redistribution"
curl -sS "$BASE/redistribution/status"  # repeat until completed/cache_ready

# 2. Start static standing-water reconstruction.
curl -i -X POST "$BASE/inundation"
curl -sS "$BASE/inundation/status"  # repeat until completed/cache_ready

# 3. Only when inundation_status == "available":
curl --fail "$BASE/inundation/depth?x=0&y=0&width=256&height=256" \
     -o standing-water-depth.f32

# Optional: cancel active reconstruction.
curl -i -X DELETE "$BASE/inundation"
```

The `JOB_UUID` and `SCENARIO_UUID` strings must be replaced with actual IDs. The `width` and `height` in the example must fit the job dimensions.

### Example completed status (illustrative)

```json
{
  "id": null,
  "job_id": "00000000-0000-4000-8000-000000000001",
  "scenario_id": "00000000-0000-4000-8000-000000000002",
  "state": "completed",
  "error": "",
  "cache_ready": true,
  "inundation_status": "available",
  "depth_available": true,
  "model": "static_standing_water_v2",
  "allocation_assumed": false,
  "columns": 3,
  "rows": 3,
  "reconstructed_volume_m3": 1000.0,
  "inundated_area_m2": 500.0,
  "inundated_cells": 1,
  "surfaces": [
    {
      "basin_id": "10000000-0000-5000-8000-000000000001",
      "water_level_m": 12.0,
      "stored_volume_m3": 1000.0,
      "inundated_area_m2": 500.0,
      "inundated_cells": 1
    }
  ]
}
```

Numbers and UUIDs above demonstrate **JSON field names only**, not a real result. `surfaces` contains active-basin UUIDs (roots or nested children) and their water elevations in the **source DEM vertical datum**, e.g. EGM2008; depths are metres. `reconstructed_volume_m3` accounts for retained basin storage, **not the entire rainfall runoff volume**. Runoff leaving the analysis domain is separately reported by redistribution.

For **pre-merge** cases with `allocation_assumed: true`, the depth GET is allowed because the independent pond geometry is consistent *given the modeled per-child water volumes*. The client must label the result conditional: redistribution does not model hydraulic timing or uniquely establish the split between sibling basins.

A new optional **C++-only** [sibling allocation sensitivity diagnostic](SIBLING_ALLOCATION_SENSITIVITY.md) can compare the cached model's ascending sibling order with an alternative descending order. It identifies leaf basins whose assigned volume is order-dependent, but is **not** part of the inundation REST status or cache and is not implied by `allocation_assumed`. In particular, `allocation_assumed: false` is not independent proof of hydraulic accuracy.

For `unresolved_partial_merge`, the completed status has `cache_ready: true`, `depth_available: false`, `inundation_status: "unresolved_partial_merge"`, empty `surfaces`, and no depth data. A depth GET returns `409 Conflict`. A completed task and a physically available raster are **different facts**.

For `failed`, the status `error` contains the processing failure (missing files, invalid input, budget, disk I/O, etc.) and `cache_ready` is false. Repair the problem and submit again.

## Raster transport and spatial registration

- Content type: `application/vnd.aowis.raster-f32le`.
- Byte format: contiguous little-endian IEEE754 **float32**, row-major, exactly `width * height * 4` bytes. No image header or compression.
- Pixel `(x,y)` is zero-based, with `(0,0)` at the analysis raster's north-west. Raster indices and georeferencing match the completed **analysis job**. Use `GET /v1/jobs/{job_uuid}` for `west`, `north`, `step_lon_deg`, `step_lat_deg`, dimensions, CRS and vertical datum.
- Each pixel is standing-water depth in **metres**, calculated as `max(0, water_surface_m - original_dem_cell_elevation_m)`. Zero only means **no retained ponded water** at that cell centre; runoff may still flow there.
- Bounds: `width/height` each 1–4096; at most **1,000,000 returned pixels**; window must lie entirely inside the stored analysis grid. Oversize or out-of-bounds requests are rejected.
- Cache reading verifies the SHA-256 digest of **each entire source raster row requested**. Only the requested rows are read and checked; the service does not load the full raster for each map pan. The window response contains only the requested columns.
- All depth windows of one completed cached analysis share one immutable spatial registration. The GUI should request windows lazily, avoid resampling into a different height datum, and never reinterpret the binary bytes as an encoded PNG.

## Workflow using the in-process C++ service

`Aowis::Hydrology::AnalysisService` exposes the same service methods in the `AOWIS::HydrologyService` library; no Qt HttpServer is required.

```cpp
QString error;
QJsonObject task = service.startStaticInundation(job_uuid, scenario_uuid, &error);
// Handle a nonempty error. Poll service.staticInundationStatus(...) later.
QJsonObject status = service.staticInundationStatus(job_uuid, scenario_uuid, &error);
if (status.value(QStringLiteral("cache_ready")).toBool() &&
    status.value(QStringLiteral("depth_available")).toBool())
{
    const QByteArray bytes = service.staticInundationDepthWindow(
        job_uuid, scenario_uuid, 0, 0, 256, 256, &error);
    // bytes is f32 little-endian with exactly 256*256 cells on success.
}
```

Methods are called on an already initialized service. A request must fit the terrain bounds. The host's existing Map instance and the Hydrology service must remain alive until outstanding tasks finish. The service destructor requests cancellation then joins workers.

## Derived persistence, integrity and restart

Per saved rainfall scenario:

- `redistribution-v1.aowbin` is the authoritative **cached volume allocation** for reconstruction.
- `inundation-v2.f32` stores float32 depth samples **only when the physical model is available**.
- `inundation-v2.json` is the versioned **commit marker**, with grid dimensions, source fingerprint, physical status, reconstructed statistics, active-basin surface metadata, and one SHA-256 hash per row.

The depth file and manifest use atomic `QSaveFile` commits, **manifest last**. An interrupted computation must not publish a partially populated new manifest. If the source file size/timestamps or redistribution cache metadata change, the fingerprint no longer matches and the derived result must be rebuilt. This fingerprint is a pragmatic **cache invalidation mechanism**, not cryptographic proof of DEM provenance. Hashes catch accidental depth-row corruption when requested; they are not an access-control feature. Corruption detected during a depth GET removes the derived manifest so a subsequent POST can regenerate the cache.

Valid derived outputs survive a service restart. Transient worker UUIDs and queue states do **not** survive restarts; after restart a previously completed cache has a `null` task ID. No separate historical progress log is persisted.

### Resource, concurrency, and cancellation policy

- The reconstruction uses the existing single-worker derived-analysis thread pool, shared with redistribution. Only one derived task executes at a time, avoiding simultaneous large terrain allocations.
- At most **eight inundation tasks** may be running/queued in one service instance. Duplicate POSTs for the same scenario reuse the **same task UUID** while the work is queued, running, or has just completed with a valid cache. The submission path rechecks a completed task's committed cache under the task-table lock, avoiding a race between its initial cache lookup and worker completion. If the cache is missing, stale, or invalidated after completion, a new POST creates a new UUID and rebuilds it.
- The raster reconstruction uses the server's `maximum_cells` limit and an additional **100,000-row** limit for per-row checksum metadata. It remains an in-memory algorithm, not an out-of-core solver.
- Cancellation is cooperative during data loading, basin reconstruction and file writing. It is not guaranteed to stop instantly. A cancellation request does not delete an already completed immutable cache.
- Derived cache creation reads all required original DEM elevations and the basin hierarchy. Subsequent window GETs read only requested raster rows plus their metadata.

## Model interpretation and next steps

The standing-water reconstruction is a stationary hydrostatic model for connected depressions. For partially formed merges, the service now draws **independent conditional child-pond surfaces** and reports `allocation_assumed: true`. This is different from claiming a unique long-term hydraulic equilibrium. `unresolved_partial_merge` remains the safe fallback for storage that cannot consistently be accounted for. Do not substitute zero depths for unavailable reconstructions.

Outstanding improvements include more physically justified inter-basin connectivity, out-of-core reconstruction for very large DEMs, consistent source revision IDs from Map, reference-solver comparisons, and integration with GUI layers. See [STATIC_INUNDATION.md](STATIC_INUNDATION.md), [RUNOFF_REDISTRIBUTION.md](RUNOFF_REDISTRIBUTION.md), and [product scope](ROADMAP.md).

### Model-version and cache transition

This version uses `static_standing_water_v2`, the `aowis-static-inundation-v2` manifest schema, and separate `inundation-v2.f32` / `inundation-v2.json` cache files. Earlier v1 depth rasters (including `unresolved_partial_merge` outcomes) are **not reused**. A new POST recomputes the v2 result from the existing authoritative redistribution cache. The redistribution archive format is unchanged; only the *derived* inundation cache is versioned. Previous v1 files may be removed by normal maintenance once no old binary uses them.
