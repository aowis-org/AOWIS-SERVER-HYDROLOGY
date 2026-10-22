# Travel-time-aware runoff forcing (experimental C++ core)

## Purpose and scope

`travelTimeInflowsFromRaster()` adds **travel delays** to the Stage 7 rainfall-runoff volume raster before feeding the coupled reservoir solver. Instead of placing water at its receiving depression or at the DEM edge immediately, the adapter traces each source cell along the **original, unfilled, unbreached D8 flow graph**, computes its path length, and shifts its explicitly prescribed runoff-time profile by a travel-time estimate.

This is an **opt-in screening model**. It does **not** update the existing static Fill–Spill–Merge service, scenario caches, HTTP endpoints, or inundation results. The [zero-travel-time temporal adapter](TEMPORAL_RUNOFF_FORCING.md) remains available and unchanged. The feature is Qt-independent and requires no additional GIS libraries beyond the Hydrology core's existing GeographicLib dependency.

**Physical status:** the caller supplies a constant *effective* routing velocity, in metres per second. Neither that velocity nor an observed travel-time distribution can be inferred reliably from GLO-30. Rainfall intensity, storm duration and effective runoff fraction likewise do not determine catchment travel time. The calculation is a deterministic translation of pulses, **not** a shallow-water, kinematic-wave, or dynamic channel-routing solver. No water is attenuated or temporarily stored along its D8 path.

## Inputs and their provenance

| Input | Units | Origin and meaning |
|---|---|---|
| `original_dem` | metres for elevation | Map-provided DEM, untouched by fill/breach; WGS84 geographic or WGS84 UTM raster |
| `hierarchy` | basin indices, metres, m³ | Depression hierarchy from **the same original raster** |
| `runoff_volume_m3` | m³ per cell | Stage 7 effective-runoff raster for the same grid and scenario |
| `reservoirs` | component indices, stage-storage curves | Exactly one independent reservoir for each depression **leaf**; in any input order |
| `time_profile` | seconds and unitless shares | Explicit user-specified event fractions and positive durations summing to the event |
| `event_duration_seconds` | seconds | Total duration of the prescribed runoff-generation profile |
| `effective_velocity_m_per_s` | m/s | **Explicit modeling assumption**, strictly positive and finite; not a DEM-derived hydraulic parameter |
| `output_interval_seconds` | seconds | Regular reporting-bin width; the final bin can be shorter |

`TravelTimeOptions` exposes the velocity and output bin width, together with maximum cell and output-interval budgets. The defaults are **1 m/s** and **60 s**, intentionally *software defaults*, **not recommended field calibration**. Specify values suitable for the experiment, and compare several plausible speeds rather than treating any one as observed.

The receiving-basin mapping uses **the same minimum-plateau and original dry-D8 rules** as the static redistributor. The adapter explicitly compares its sum of routed source volumes per leaf and at the direct DEM boundary against an independently computed static redistribution. A mismatch is an error, not a silent diversion of water. Internal parent/merge nodes are never injected as additional reservoirs; static `overflow_m3` is not reused as an inflow, avoiding double-counted water.

## Calculation

For every cell *k* with nonzero runoff, let its D8 path from the source cell centre to its terminal leaf minimum or DEM-boundary outlet contain edge distances `d_k,e` in **metres**. Geographic grids use GeographicLib WGS84 geodesic distances; UTM distances are adjusted for projection scale at the raster row. The total path travel time is

\[
\tau_k = \frac{\sum_e d_{k,e}}{v},\qquad v>0.
\]

This is a fixed travel-time translation, independent of water depth, slope changes during flooding, channel bankfull discharge, backwater, and routing in real pipes/culverts. It takes the *dry* D8 route and does not dynamically reroute water that overtops terrain.

For a spatially uniform prescribed storm-time profile, let `f_j` denote the fraction of source-cell runoff produced uniformly over storm interval `[a_j,b_j)`. Each cell emits `V_k f_j` m³ at constant mean rate `V_k f_j / (b_j-a_j)` while that source interval is active. The receiving destination sees the **same pulse shifted** to `[a_j + τ_k, b_j + τ_k)`. For output bin `[t_n,t_{n+1})`, the received volume is

\[
V_{k,j,n} = V_k f_j\frac{\operatorname{length}([t_n,t_{n+1})\cap[a_j+\tau_k,b_j+\tau_k))}{b_j-a_j}.
\]

The output bin range covers the full event plus the **maximum travel delay among cells with positive runoff**. It includes a post-rainfall tail and never discards delayed water merely because rainfall ended. `maximum_source_travel_seconds` and `output_duration_seconds` describe this range. A zero-runoff event retains the original storm duration.

A source already at its terminal minimum or outlet has zero path travel time. D8 corner-to-corner links use the **diagonal** distance, not one cell width. Terrain cells lying inside the analysis window but draining through the cut DEM boundary are reported as **direct boundary runoff**, not sent to a reservoir or counted as a later hydraulic outlet discharge.

### Volume conservation

The adapter checks both the destination partition and the output-bin totals:

\[
\sum_k V_k = \sum_{i\in\mathrm{leaves}}\sum_n I_{i,n} + \sum_n B_n.
\]

`I_i,n` is the volume delivered to basin *i* during output interval *n*; `B_n` is **direct dry-D8** runoff leaving the raster boundary. The `TemporalRunoffForcing` fields retain their existing meanings, including `generated_runoff_m3`, `delivered_to_basins_m3`, `direct_boundary_outflow_m3`, and `mass_balance_error_m3`. The bins of `basin_inflows` may outlast `event_duration_seconds`.

**Do not count `B_n` as hydraulic reservoir outlet flow.** When using `routeCoupledReservoirsWithOutlets()`, boundary discharge from modeled reservoirs is another, nonoverlapping contribution. The basin inflow vectors can be passed directly to either closed or open coupled routing.

## Numerical example

Consider a single basin, a 2-minute uniform runoff-generation event and two contributing cells with 60 m³ of effective runoff each:

- Cell A is at the basin minimum: travel delay 0 s.
- Cell B has a 100 m D8 path and an **assumed** effective speed 0.5 m/s: travel delay 200 s.

With 60-second reporting bins, A delivers 30 m³ in each of minutes 0–1 and 1–2. B delivers 30 m³ in each of minutes 3:20–4:20 and 4:20–5:20, split by bin overlaps. The total 120 m³ reaches the basin, but the hydrograph peak and timing differ sharply from the zero-delay adapter. Those timings reflect the assumed speed, not measured flood arrival.

| Reporting interval | Cell A | Cell B | Basin total |
|---|---:|---:|---:|
| 0–60 s | 30 m³ | 0 | 30 m³ |
| 60–120 s | 30 m³ | 0 | 30 m³ |
| 120–180 s | 0 | 0 | 0 |
| 180–240 s | 0 | 20 m³ | 20 m³ |
| 240–300 s | 0 | 30 m³ | 30 m³ |
| 300–320 s | 0 | 10 m³ | 10 m³ |
| **Total** | **60 m³** | **60 m³** | **120 m³** |

The adapter reports the last interval's actual shorter duration (20 s), rather than a misleading full-minute bin.

## C++ example

```cpp
#include <aowis/hydrology/travel_time_inflows.h>

using namespace Aowis::Hydrology;

// original_dem, hierarchy, cell_runoff_m3 and reservoir_list originate
// from a single consistent Map-backed terrain analysis and rainfall event.
const std::vector<TemporalRainfallInterval> profile = {
    {600.0, 0.30},
    {1200.0, 0.70}
};
TravelTimeOptions timing;
timing.effective_velocity_m_per_s = 0.5; // explicit experiment assumption
timing.output_interval_seconds = 60.0;

const TravelTimeRunoffForcing lagged = travelTimeInflowsFromRaster(
    original_dem, hierarchy, cell_runoff_m3, reservoir_list,
    profile, 1800.0, timing);

// Provide separately sourced/calibrated hydraulic connections and outlets.
const CoupledRoutingResult routed = routeCoupledReservoirsWithOutlets(
    reservoir_list, connections, outlets, lagged.forcing.basin_inflows);

// lagged.forcing.direct_boundary_outflow_m3_by_interval is kept separate.
// routed.snapshots.back().cumulative_boundary_outflow_m3 describes only
// modeled reservoir outlets.
```

For a simple closed network, pass `lagged.forcing.basin_inflows` to `routeCoupledReservoirs()` instead. When the hierarchy contains no leaves, the function can still return only direct-boundary volumes, and no coupled reservoir run is required.

## Performance, validation, and limitations

- The implementation runs the existing static spatial validation/routing, then builds the dry D8 graph for travel-time calculation. It uses one topological pass to propagate destinations and path times **upstream**. This extra dry-D8 analysis can be significant for metropolitan grids and is a candidate for future shared-flow preprocessing and caching.
- Pulses are accumulated with partial-bin contributions and a prefix-summed difference array for whole bins. Work is approximately `O(cells × storm_intervals + output_bins × (leaf_basins + 1))`, plus spatial routing and geographic distance setup; long pulses do **not** require looping over every crossed output bin for every cell.
- Work caps include up to 10 million raster cells by default, 10,000 source intervals, 10,000 output intervals, 5 million output interval–destination entries, and 300 million source-cell–interval combinations. Each limit is checked before large output allocation. The long-double accumulation arrays can still consume significant memory. `maximum_raster_cells` and `maximum_output_intervals` may be lowered by the caller; the fixed caps are not service memory guarantees.
- The event fractions must sum to 1 and durations must sum to the event duration; negative/nonfinite runoff, missing terrain coverage, mismatched basin topology, duplicate/missing reservoir leaves, invalid velocity and unsupported work budgets are rejected. Cancellation is cooperatively checked during graph preparation, distance computation, pulse deposition and bin assembly.
- All elevation datum values remain unchanged; travel time uses **horizontal distance** only. The adapter does not correct for vertical drainage distances, steep surface roughness, wetness-dependent speed, stream network routing, seasonal land cover or engineered storm drains.
- Reporting bins contain **interval-integrated m³**, not m³/s. For mean inflow discharge, divide a bin's `inflow_volume_m3` by its `duration_seconds`. Smaller bins improve output timing resolution, not underlying physical calibration.
- This is not a flood-wave or travel-time probability distribution: every source cell is shifted by one deterministic path delay. Real hydrographs commonly spread, slow, and attenuate during transport, especially near flatter surfaces and junctions.
- The analysis boundary can truncate real upstream catchments. Direct runoff recorded at that clipped boundary is an **analysis-domain** exit, not proof of an existing drainage outlet. Do not use the model to claim real-world safety or street-level flooding based solely on Copernicus GLO-30.

### Testing

`tests/test_travel_time_inflows.cpp` covers a synthetic WGS84 UTM basin, a zero-delay sink, single-step arrival delay, speed sensitivity, direct boundary travel, an entirely dry storm, nonuniform runoff-time fractions, bin and work budgets, malformed input, cancellation, and coupled-routing consumption. The standard `./compile_linux_tests.sh --both` workflow includes this test as `aowis-hydrology-travel-time`.

See also [Temporal runoff forcing](TEMPORAL_RUNOFF_FORCING.md), [Coupled reservoir routing](COUPLED_RESERVOIR_ROUTING.md), [Boundary outlets](BOUNDARY_OUTLETS.md), and [Depression hierarchy](DEPRESSION_HIERARCHY.md). The opt-in coupled-simulation service now supports this adapter through `scenario_forcing` requests and persists the **assumed velocity, output bin width, profile, source DEM and rainfall-source checksums, calibrated control configuration, and simulation identity**; see [Scenario-driven forcing](SCENARIO_COUPLED_FORCING.md). Existing REST *static* redistribution caches deliberately remain unchanged.
