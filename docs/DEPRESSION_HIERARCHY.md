# Depression hierarchy and basin storage

The hydrology core includes `depression_hierarchy.h` with a **raster-centre bathtub** model derived from the **original, immutable elevation grid**. It does not use the Priority-Flood-conditioned D8 surface, breach cuts, or invented flat gradients. Consequently, potential fill-depth diagnostics from the routing engine and actual *connected-basin storage* calculations must not be conflated.

## Algorithms

`buildDepressionHierarchy(source, reject_interior_nodata, cancel)` processes valid cells in ascending elevation, connecting them with 8-neighbour topology as they become submerged. Contiguous equal-elevation plateau cells are activated together so one flat-bottomed depression produces one basin, rather than thousands of artificial minima.

- A **leaf** starts at a locally isolated minimum.
- When two closed flooded components meet at a saddle, a **parent basin** is formed with those components as children.
- When a closed component connects to an open boundary, its final spill elevation and pre-spill storage capacity are recorded.
- For every basin: internal array index, parent/children, minimum cell/height, formation elevation, spill cell/height, exit-to-boundary flag, area and capacity at spill.
- For each component, capacity is computed from cell-area-weighted heights on the **unmodified DEM**. The accumulator uses heights *relative to its minimum* to reduce catastrophic cancellation at large absolute elevations with shallow basins.
- Missing terrain can be explicitly rejected (default) or treated as open terrain-data boundaries. Such open boundaries are **not** evidence of a real water outlet.
- Cancellation is checked during preprocessing. Memory and sorting scale with the input cell count; the routine is *in-memory*, not yet out-of-core.
- **Row-area memoization:** Raster cells in the same row share the same geographic
  ellipsoidal-polygon area, or the same representative UTM projection-scale
  area under this raster geometry contract. The hierarchy evaluates the
  unchanged `rasterRowCellAreaM2()` formula once per raster row and reuses it
  when activating cells. This is an implementation optimization, **not** a
  replacement of GeographicLib with a spherical or planar approximation.
  `basinStorageAtLevel()` uses a lazy row cache during its bounded flood fill,
  so queries visiting only a few rows don't evaluate every row in the raster.
  Different rows retain distinct areas; this is checked on EPSG:4326 and
  WGS84 UTM test grids. Neither the basin topology nor the archive format
  changes, and no existing job must be regenerated.
- **Singleton-height fast path:** An elevation group containing one cell
  bypasses the temporary hash map and plateau-root sort. All cells still go
  through the same component connectivity, saddle detection, merge, and basin
  finalization routine. For multi-cell equal-elevation groups, the existing
  plateau grouping and stable root ordering are retained. Reusable neighbor
  and closed-component vectors reduce repeated allocations. This changes no
  basin serial format, float arithmetic, boundary treatment, or API.
  Synthetic mixed/near-unique elevations retain matching basin records;
  measured GLO-30 performance and its limitations are documented in
  [benchmark findings](BENCHMARK_FINDINGS.md).

`basinStorageAtLevel(source, hierarchy, index, water_level_m, max_visited_cells, cancel)` estimates the connected area and stored volume at a queried water level. Its accepted interval is **from the basin's formation elevation through its spill elevation**. At exactly the spill level, it returns the stable pre-spill statistics produced by the elevation sweep (saddle-height cells add no capacity). Levels above spill are rejected; the caller must later apply the Fill–Spill–Merge distribution algorithm rather than ignoring the spill.

The on-demand query intentionally bounds the number of visited cells and does not precompute large volume/area tables for every nested basin. Completed analysis jobs already persist basin summaries and expose bounded, sampled storage curves through the service and REST APIs, without rerunning terrain analysis for each rainfall scenario.

## What this represents—and what it does not

All areas use GeographicLib-backed metric cell areas, while heights remain in the source vertical datum (EGM2008 for GLO-30). The model assumes horizontal water surfaces, 8-neighbour raster connectivity, impermeable terrain and **no timing**. It does **not** simulate rain, infiltration, culverts, pump stations, water velocities, dynamic hydraulic backwater or channel stage.

A basin hierarchy computed for a clipped DEM uses the **analysis boundary as its exterior**. Results can be truncated or have artificial spill levels if upstream terrain is missing or if the raster ends inside a real watershed. GLO-30 is a DSM with buildings and vegetation and has limited spatial accuracy for small urban depressions.

Stable AOWIS model entities use UUIDs. The basin indices returned by this core API are *internal, reproducible array indices*, not persistent model entity identifiers.

## Example: two nested minima

A 7 × 5 elevation grid with a 16 m rim, two 10 m pits and a 12 m saddle produces two leaf basins at 10 m. They merge into a parent at 12 m. The parent then spills over the outer rim at 16 m. Before that external spill, the water volume is the combined cell-centre volume from all flooded 10 m and 12 m cells. The two lower basins retain distinct pre-merge capacities; their totals are not simply added to a parent's overall capacity a second time.

## Verification

`aowis-hydrology-depressions` tests the simple-pit volume, two-pit hierarchy, formation-level restrictions, flat plateaus, missing elevations, cancellation, resource limits, and 30 randomized landscapes checked against independent sub-spill flood fills. Run through `./compile_linux_tests.sh --both` or through CTest with label `hydrology`.

## Persistent basin results and queries

Completed jobs persist a checksum-validated little-endian `basins.aowbin`
archive (format `AOWHDH01`). It records each node, spill metadata and physical
capacity; parent relations reconstruct child lists on load. Source elevations
remain stored independently as `elevation.f32` and are loaded for explicit
water-level sampling. The original DEM is never recomputed from the
conditioned or breached routing surface. Corrupt, oversized, missing or
mismatched hierarchy artifacts cause explicit errors. Older completed jobs without a basin archive need to be rerun.

All basin entities exposed to a client use deterministic, job-scoped UUIDs.
The library `AnalysisService` exposes `basins(job, offset, limit)`,
`basin(job, basin_uuid)`, `basinStorage(job, basin_uuid, level)`, and
`basinStorageCurve(job, basin_uuid, samples)`. The HTTP adapter provides
matching `/v1/jobs/{id}/basins` and `/basins/{basin-uuid}` endpoints with
`/storage?level_m=` and `/curve?samples=` children. Pages contain at most
100 basins; sampled curves contain 2–16 levels, including formation and
spill. Storage queries use a bounded 2-million-cell flood traversal.
Hierarchy construction occurs before marking the terrain job complete, and
its results remain available following a service restart.

## Limits and open questions

Hierarchy construction and persistence have been benchmarked on real GLO-30
regions; further profiling on larger or different terrain remains useful.
Independent reference comparisons, scalable queries for very large connected
basins and source-revision provenance still need work. Static redistribution
and conditional static depth reconstruction are separate available operations;
these storage curves themselves do not predict flood timing or discharge.

## Relationship to rainfall redistribution

Static redistribution uses this same saved **original-elevation** hierarchy, not the filled
routing terrain. Each leaf's `capacity_at_spill_m3` acts as its maximum
independent storage before water can cross the saddle to sibling basins or to
its parent. An internal parent capacity **includes** its children: summing
all individual capacities or stored-subtree results would double-count water.
Only root basins are summed in the static redistribution total. The actual scenario endpoint
is described in [RUNOFF_REDISTRIBUTION.md](RUNOFF_REDISTRIBUTION.md); these
static storage curves alone are not evidence of stormwater inundation.
