# DEM conditioning and flat-area flow routing

Hydrology uses Map-owned terrain data (Copernicus GLO-30 for the initial product).
The computation never writes elevation changes back to Map. Its **terrain** and
**drainage graph** are separate from rendering tiles or map-server caches.

## Conditioning policy implemented today

The analysis accepts `fill_depressions=true` or `false`, and a separate,
optional `breach_depressions` switch. These are routing and conditioning
policies, not rainfall settings:

- **Fill (default)**: A boundary-seeded 8-connected Priority-Flood computes
  the minimum *filled water-level surface* needed to connect interior low
  areas to an open analysis boundary. The DEM's heights are not incremented
  by artificial epsilon slopes. Flat routing is expressed entirely through
  graph edges. The resulting downstream graph can traverse filled flats and
  yields downstream accumulation and catchments.
- **Preserve** (`fill_depressions=false`): D8 uses the original elevations.
  Flat components with an available lower outlet are routed to that outlet;
  flat closed components are assigned one deterministic internal sink. A
  sink is not automatically considered connected to an outside outlet.

**Constrained local breaching (optional, disabled by default)** first
identifies genuine internal pits in the *unfilled* D8 graph. For each pit,
a bounded least-excavation search may lower a short obstruction to the
pit's original elevation, but only if it can reach a lower cell whose original
Priority-Flood level is already at or below the pit elevation. This avoids
turning a closed catchment into an invented outlet by cutting a barrier that
still drains into another closed basin. A successful trench can have level
segments; flat routing establishes its drainage without artificial epsilon
height adjustments. No source elevations are overwritten.

Breaching is optional and uses explicit limits:

| JSON field | Default | Meaning |
| --- | ---: | --- |
| `breach_depressions` | `false` | Try local breaching before optional residual filling |
| `maximum_breach_cut_depth_m` | 3 m | Maximum cumulative lowering of any cell from original DEM |
| `maximum_breach_path_cells` | 8 | Maximum number of 8-neighbor steps from pit to an existing lower outlet (2–32) |
| `maximum_breach_excavation_m3` | 100,000 m³ | Maximum excavation volume within a single candidate path |
| `maximum_breach_candidates` | 2,000 | Maximum examined closed pits; exceeding it fails the job explicitly |

The least-cost objective minimizes cut volume over the bounded path,
with shortest-path tie breaking. Existing cuts can be reused, and the
cumulative per-cell depth limit is measured against the *original* DEM.
This is a **conservative local notch strategy**, not a general unrestricted
least-cost channel or a culvert detector. It cannot establish that a road,
berm or high DSM cell should actually be removed. Wider or deeper obstructions
remain intact if they exceed limits.

`breach_depressions=true, fill_depressions=false` preserves any sinks that
cannot be breached. With both enabled, accepted local cuts are applied
first and remaining closed depressions are filled to their spill surfaces.
The fill-depth diagnostic continues to describe the unmodified original DEM,
not the post-breach routing surface. In GLO-30, avoid enabling breaching
indiscriminately across a whole metropolitan region without examining
large apparent artifacts and the actual terrain uncertainty.

## Physical heights and diagnostics

The core's `AnalysisResult` contains:

| Field | Meaning |
| --- | --- |
| `source.elevations_m` | Original, immutable DEM sample heights |
| `conditioned_elevations_m` | Final routing surface: original elevations, optionally locally cut and/or filled |
| `potential_fill_depth_m` | Height of the *potential filled* surface minus original height (m), in either mode |
| `downstream` | D8 drainage graph; equal-height edges on flats are permitted, graph is acyclic |
| `conditioned_cell_count` / `maximum_fill_m` | Cells and maximum raise actually applied to routing surface |
| `potential_fill_cell_count` / `maximum_potential_fill_m` | Cells and maximum raise that would be required for full fill, even if mode is Preserve |
| `potential_fill_volume_m3` | Sum of cell area × potential fill depth over all valid cells |
| `breach_cut_depth_m` | Physical cut depth applied in the *final* routing DEM (metres; zero elsewhere) |
| `accepted_breach_paths` / `rejected_breach_candidates` | Local path diagnostics, not counts of confirmed culverts |
| `breach_excavation_m3` | Total applied excavation (physical cut depth × cell area) |
| `maximum_breach_cut_m` | Maximum applied cut depth from original DEM |

The server persists these fields in job metadata and the source, conditioned,
potential-fill-depth and breach-cut-depth raster layers. The potential-fill-depth
layer is available through
`/v1/jobs/ID/raster/potential-fill-depth?x=...&y=...&width=...&height=...`.
The cut-depth raster uses the equivalent `breach-cut-depth` layer name.
Both are float32 little-endian metres, row-major. Uncovered cells are NaN when
using the core directly. The production service currently rejects all
incomplete DEM coverage.

**`potential_fill_volume_m3` is a terrain-conditioning diagnostic, not an
estimated stormwater volume or prediction of flooded water.** It computes the
volume to raise all depressions to a boundary-connected surface under the
current domain assumptions. Real stormwater capacity depends on *nested*
basin topology, existing water, outlet elevations, rainfall polygon,
infiltration and additional hydraulic connectivity. The separate depression
hierarchy, storage curves and static Fill–Spill–Merge-style redistribution
provide the topographic accounting used by rainfall-volume scenarios.

## Numerical and domain assumptions

- Uses 8-connected raster topology, with boundary pixels initially considered
  open outlets. Bounding the region too closely can create false drainage
  exits. Computational terrain bounds must include the relevant catchments.
- The Priority-Flood and flat propagation have linear or near-linear memory
  overhead in the number of cells. No disk-backed/out-of-core implementation
  is present yet.
- Physical cell areas/directions come from the GeographicLib-backed raster
  geometry layer, not a fixed nominal 900 m² assumption for GLO-30.
- Exact float-height equality is used to identify plateaus. The result is
  deterministic for a given raster and settings; no random slopes are added.
- Missing interior cells are rejected by default. When explicitly allowed in
  the core, NoData-adjacent cells are treated as open drainage boundaries.
- GLO-30 is a digital surface model; buildings and vegetation can introduce
  artificial sinks, barriers and local height differences. Conditioning
  diagnostics are indicators for inspection, not proof of physically correct
  drainage paths.

## Verification

Synthetic tests cover depression fill heights and physical fill volume,
flat closed basins and sink uniqueness, preserving terrain, outlet-connected
flats, geographic and UTM cell metrics, random DEM flow-graph acyclicity,
NoData rejection and cancellation. They run under the existing Hydrology
Debug/Release build scripts. Comparison against GRASS and WhiteboxTools,
including cross-tool breaching comparisons, and wide-obstruction channel
routing remain future work. Current tests also cover bounded notch acceptance/rejection,
excavation volume, nested physical basins, and candidate-budget exhaustion.
