# Runoff redistribution and basin overflow

**Status:** first Stage 8 implementation (static, volume-only Fill–Spill–Merge-style approximation). Implemented in `include/aowis/hydrology/redistribution.h`, `src/core/redistribution.cpp`, the reusable Qt service, and the REST adapter. This is **not** a hydrodynamic flood model.

## 1. What problem this answers

Given an **already completed terrain analysis** and an **already saved rainfall scenario**, the model estimates:

1. How much effective rainfall runoff gravitates directly out of the selected DEM window.
2. How much runoff collects in closed terrain depressions.
3. How water overflowing a filled depression can fill another basin across a saddle.
4. How much water remains inside the overall depression hierarchy versus passes out of the DEM window.
5. Whether the reported volumes satisfy conservation of mass.

It intentionally does **not** calculate storm arrival times, flow velocities, discharge hydrographs, pipe transport, culverts, pumps, street runoff resistance, buildings, infiltration over time, or an inundation-depth raster. The rainfall scenario's `duration_seconds` does not influence routing: the entire event's effective runoff volume is routed in one static step. Treat it as a **screening approximation**, not a flood forecast.

## 2. Source of truth and dependencies

| Input | Origin | Rule |
| --- | --- | --- |
| Elevations and grid geometry | Completed job, `elevation.f32` | Original, unmodified source DEM from Map |
| Basin topology and capacities | Completed job, `basins.aowbin` | Calculated from original DEM; persisted with checksummed archive |
| Runoff volume per cell | Scenario, `runoff.f64` | Derived in Stage 7 from clipped rainfall polygons, depth, and runoff fraction |
| Flow graph | Calculated from original DEM on first scenario redistribution | Unfilled, unbreached D8: a real closed depression stays a sink; later queries reuse cached results |

The persisted job's conditioned D8 flow graph is **not** reused here, because a filled routing surface deliberately gives a sink an artificial drainage connection. Such a link is useful for watershed tracing but must not magically empty a basin before reaching its saddle.

**Map remains the only terrain provider**. Queries use saved data and do not download Copernicus data again. No new GUI, Map, Standalone, or SWMM dependency is introduced.

All DEM cells must contain finite elevations for this increment. Missing cells are rejected: interpreting a hole as an outflow could understate storage catastrophically. Basin archives with unresolved spill elevations are also rejected. A clipped DEM boundary is always a model boundary, **not proof that a real outlet exists there**.

## 3. Calculation, in order

### A. Generate surface runoff (Stage 7)

For a DEM cell with rainfall polygon coverage fraction `f`, physical area `A` in m², event depth `P` in mm, and effective runoff fraction `r`:

`generated_runoff_m3 = A * f * (P / 1000) * r`

Overlapping polygons are unioned (not counted twice). Water in the complement `1-r` is reported by Stage 7 as *retained or lost* before this redistribution starts. It does **not** enter Stage 8's conservation equation. See [RAINFALL_SCENARIOS.md](RAINFALL_SCENARIOS.md).

### B. Dry-surface D8 transport

Each cell's runoff moves along the unfilled, unbreached original-DEM D8 graph. Flat terrain is routed topologically without changing heights. Every path ends at one of:

- an edge of the DEM: counted as `direct_boundary_outflow_m3`;
- an interior closed sink: assigned to the corresponding **leaf** depression's minimum plateau.

The calculation is acyclic and accumulates contributions in topological order. It assumes all generated runoff can reach its terminal point during the event, regardless of travel length, ground roughness, or event duration. There are no per-pixel water heights along a path.

### C. Fill, spill, and merge

The source DEM's 8-connected elevation-ordered basin hierarchy has **leaf minima**, **spill saddle heights**, and **parent basins** where previously separate ponds meet.

- Incoming volume first fills the local leaf to its physical `capacity_at_spill_m3`.
- A leaf's excess passes to its parent merge node.
- That excess first fills any **underfilled sibling** connected at the same saddle.
- After all connected child cavities reach the merge saddle, remaining water contributes to the parent basin's additional storage, up to its own spill capacity.
- Excess from the top-level/root basin passes out of the domain as `basin_boundary_outflow_m3`.

Sibling allocation is **deterministic** (child order in the stored hierarchy). Where several recipients have the same spill saddle, there is no hydraulic timing or channel-capacity model that could establish which receives water first. **A particular partially filled sibling is therefore not a calibrated prediction. The static inundation API can convert assigned per-sibling volumes to **conditional** independent pond surfaces, flagged with `allocation_assumed: true`. That is not a unique hydraulic solution.** The aggregate volume and capacity model is the intended product of this stage.

**Sibling-allocation diagnostic (C++ only).** To expose this ambiguity, the
optional `assessSiblingAllocationSensitivity()` reruns redistribution with
ascending and descending child visitation. It lists **leaf** basins whose
retained volume differs and verifies that global storage/outflow do not.
Neither alternative is a calibrated hydraulic solution; agreement between the
two is not proof of uniqueness. Normal scenario runs, their persisted cache,
and REST responses retain the existing ascending-order result. See
[SIBLING_ALLOCATION_SENSITIVITY.md](SIBLING_ALLOCATION_SENSITIVITY.md) for
usage, analytical tests, runtime costs and interpretation.

**Capacity-weighted alternative (C++ only).**
`redistributeRunoffWithSiblingPolicy(..., RemainingCapacityWeighted)` removes
child-index priority by splitting eligible overflow in proportion to each
receiving child's remaining storage below spill. This is a reproducible
**allocation heuristic**, not an actual hydraulic flow model. It does **not**
change persisted scenarios, cache contents, asynchronous REST jobs or the
`allocation_assumed` flag for conditional pre-merge inundation. Compare it
explicitly rather than substituting it silently for the default. The formula,
API and limitations are documented in
[CAPACITY_WEIGHTED_SIBLING_ALLOCATION.md](CAPACITY_WEIGHTED_SIBLING_ALLOCATION.md).

### D. Storage aggregation and accounting

`stored_in_subtree_m3` for an **internal parent** already includes water stored in all its descendants. Never add all basin rows together. To obtain total retained water, **sum only root basins**, exactly as the service does.

The mass-balance equation is:

`generated_runoff_m3 = basin_storage_m3 + direct_boundary_outflow_m3 + basin_boundary_outflow_m3 + mass_balance_error_m3`

and

`total_boundary_outflow_m3 = direct_boundary_outflow_m3 + basin_boundary_outflow_m3`

`mass_balance_error_m3` is the signed residual `generated - stored - total_outflow`; a nonzero value near floating-point precision is normal. The C++ implementation throws when its absolute value exceeds `1e-8 * max(1, generated_runoff_m3)`. This checks **numerical conservation**, not physical accuracy.

## 4. REST API

All IDs are UUIDs. A terrain job must be `completed` and contain its basin hierarchy. The scenario must already exist (Stage 7).

```http
POST /v1/jobs/{job-uuid}/rainfall-scenarios/{scenario-uuid}/redistribution
GET /v1/jobs/{job-uuid}/rainfall-scenarios/{scenario-uuid}/redistribution/status
GET /v1/jobs/{job-uuid}/rainfall-scenarios/{scenario-uuid}/redistribution?offset=0&limit=50
DELETE /v1/jobs/{job-uuid}/rainfall-scenarios/{scenario-uuid}/redistribution
```

`offset` defaults to 0 and must be nonnegative. `limit` defaults to 50 and accepts 1–100. Pagination changes only the **basin rows returned**, not the computed global totals. HTTP POST schedules computation in a bounded background queue and **atomically caches** its result per scenario. Poll `GET .../status` until `state=completed` and `cache_ready=true`. HTTP GET results is **cache-only**, returns `409 Conflict` until a valid cache exists, and never recomputes a missing or invalid cache. Re-submit POST to repair a stale/corrupt derived cache. Subsequent pages and requests after restart reuse the cache without recalculating D8 or reloading the DEM. HTTP result `cache_hit` is therefore true; for legacy synchronous in-process calls it is false when a new calculation ran. Cache design, binary format, validation limits and recovery are documented in [REDISTRIBUTION_CACHE.md](REDISTRIBUTION_CACHE.md) and
[ASYNCHRONOUS_REDISTRIBUTION.md](ASYNCHRONOUS_REDISTRIBUTION.md).

Example response values below illustrate the **schema and accounting**, not a specific real location:

```json
{
  "model": "static_fill_spill_merge_volume_v1",
  "job_id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
  "scenario_id": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
  "generated_runoff_m3": 1200.0,
  "basin_storage_m3": 800.0,
  "direct_boundary_outflow_m3": 250.0,
  "basin_boundary_outflow_m3": 150.0,
  "total_boundary_outflow_m3": 400.0,
  "mass_balance_error_m3": 0.0,
  "domain_boundary_is_artificial": true,
  "cache_hit": true,
  "total_basins": 1,
  "offset": 0,
  "limit": 50,
  "has_more": false,
  "basins": [
    {
      "basin_id": "cccccccc-cccc-4ccc-8ccc-cccccccccccc",
      "parent_id": null,
      "direct_inflow_m3": 950.0,
      "stored_in_subtree_m3": 800.0,
      "overflow_m3": 150.0,
      "at_spill": true
    }
  ]
}
```

Pagination may return fewer `basins` entries than `total_basins`; in that case `has_more` indicates whether additional pages exist. `overflow_m3` for an internal basin is the amount passed upward during bottom-up aggregation; **do not add these values across all nodes** as a second outflow measure. The only definitive out-of-domain amount is `total_boundary_outflow_m3`.

`parent_id` is `null` at the root. For each entry, `direct_inflow_m3` is nonzero only for a leaf sink, `stored_in_subtree_m3` includes descendants, `overflow_m3` is volume passed up the tree, and `at_spill` means that basin reached its recorded storage capacity. All quantities are m³ except flags and identifiers.

### Example request sequence

```bash
# 1. Create terrain analysis; wait until state == completed.
curl -X POST http://127.0.0.1:8123/v1/jobs \
  -H 'Content-Type: application/json' \
  -d '{"west":8.0,"south":50.0,"east":8.02,"north":50.02,"resolution_m":30}'

# 2. Create a rainfall scenario on that finished job; save returned scenario UUID.
curl -X POST http://127.0.0.1:8123/v1/jobs/JOB_UUID/rainfall-scenarios \
  -H 'Content-Type: application/json' \
  -d '{"name":"Example event","depth_mm":35,"duration_seconds":3600,"effective_runoff_fraction":0.65,"geometry":{"type":"Polygon","coordinates":[[[8.005,50.005],[8.015,50.005],[8.015,50.015],[8.005,50.015],[8.005,50.005]]]}}'

# 3. Query the static redistribution result.
curl 'http://127.0.0.1:8123/v1/jobs/JOB_UUID/rainfall-scenarios/SCENARIO_UUID/redistribution?offset=0&limit=50'
```

These requests require a configured Map server for the *initial* terrain-analysis job. Subsequent scenario and redistribution reads use Hydrology's persisted DEM. In standalone, use the equivalent `AnalysisService::rainfallRedistribution()` call with the same UUIDs, offset, and limit—no loopback HTTP is necessary.

### Errors and missing features

- Invalid UUID, noncompleted analysis, invalid pagination, corrupt or missing raster/archive: the query fails rather than returning fabricated zeros.
- DEM with any missing elevation: query fails in this increment, including missing boundary pixels.
- Large valid DEMs currently need in-memory source, dry-flow graph, routing accumulators and hierarchy; this operation is **not out-of-core**.
- Asynchronous redistribution tasks have UUID task IDs and bounded background execution. The result is persisted as a derived, disposable binary cache separate from authoritative rainfall input and terrain source. REST GET never initiates a new long-running solve.
- Static inundation REST depth windows now exist through a separate asynchronous task and derived depth cache. They represent standing-water estimates subject to the redistribution allocation assumption, not a hydraulic forecast. See [INUNDATION_SERVICE.md](INUNDATION_SERVICE.md), [STATIC_INUNDATION.md](STATIC_INUNDATION.md) and [SIBLING_ALLOCATION_SENSITIVITY.md](SIBLING_ALLOCATION_SENSITIVITY.md).

## 5. Pure C++ library

```cpp
#include <aowis/hydrology/redistribution.h>

// source: original unmodified ElevationGrid
// hierarchy: buildDepressionHierarchy(source) or the corresponding archive
// scenario_runoff: one nonnegative double (m3) per source DEM cell
const Aowis::Hydrology::RunoffRedistribution result =
    Aowis::Hydrology::redistributeRunoff(source, hierarchy, scenario_runoff);

// Use the summary for mass conservation, not a sum of all basin rows.
const double retained_m3 = result.basin_storage_m3;
const double leaving_domain_m3 = result.total_boundary_outflow_m3;
```

The pure C++ function accepts an optional cancellation flag (`const std::atomic<bool>*`). It performs its own dry-D8 analysis; therefore it has CPU and RAM costs in addition to the existing DEM and basin archive. `AOWIS::HydrologyService` is the transport-independent Qt job interface; the REST server only translates its output to HTTP JSON.

## 6. Accuracy and interpretation

**What the output can help screen:** which terrain sinks might retain runoff volumes, whether a hierarchy of neighboring basins has enough theoretical storage to retain a chosen event, and how much volume can leave the selected DEM window under simplified gravity-driven assumptions.

**What the output cannot substantiate:** exact flooded street locations, inundation depths, building impacts, reservoir safety, times to overflow, or downstream flows outside the map extent. A 30 m satellite DEM may omit walls, kerbs, culverts and local drainage that dominate urban flooding. Long upstream catchments outside the requested DEM are not included. Small levees or terrain artifacts may substantially alter the results. No flow velocity, volume attenuation along routes, delayed interception, drainage-network capacity, or storm temporal distribution is included.

**Boundary choice matters.** Water leaving the chosen DEM bounding box is counted as *out of domain*, not lost from the physical watershed. For meaningful watershed decisions, expand the analysis domain to include the contributing area and downstream terrain. The model does not automatically fetch or grow the area.

**Model comparison matters.** This initial solver is an AOWIS approximation inspired by Fill–Spill–Merge, *not* a claim of numerical equivalence with a published Fill–Spill–Merge implementation. The tests exercise volume conservation and analytic pits, but large reference DEM comparisons and observed flood validation remain work in [ROADMAP.md](ROADMAP.md).

## 7. Verification and next improvements

Automated tests (`aowis-hydrology-redistribution`) cover zero/partial/full pit storage, parent/sibling redistribution, boundary overflow, cancel/reject behavior, randomized landscapes, and monotonic total storage under increasing rainfall. Service tests exercise the persisted rainfall-raster input and UUID-based result interface, including after restart. Normal Debug and Release regression commands:

```bash
./compile_linux_tests.sh --both
```

Versioned, paginated scenario caching is implemented (see [REDISTRIBUTION_CACHE.md](REDISTRIBUTION_CACHE.md)). Background execution with cooperative cancellation is implemented, but progress percentage/ETA and durable task histories are not. A first conservative C++ water-level/depth reconstruction for isolated and fully connected roots is described in [STATIC_INUNDATION.md](STATIC_INUNDATION.md); it can reconstruct independent pre-merge child-pond surfaces, explicitly flags the deterministic allocation assumption, and rejects inconsistent unassigned storage. Important remaining work: more generally reliable water-level/depth maps, explicitly bounded sensitivity-query service support, improve spill routing beyond the rectangular DEM domain, add robust cache content fingerprints and multi-process coordination, and benchmark/compare real DEMs with external hydrology software.
