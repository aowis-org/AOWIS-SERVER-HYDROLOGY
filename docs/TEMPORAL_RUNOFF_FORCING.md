# Timed runoff forcing for coupled basin routing

## Scope and modeling status

`temporalInflowsFromRaster()` and `temporalInflowsFromRedistribution()` are **Qt-independent, opt-in adapters** linking existing **Stage 7 event-total effective-runoff rasters** to the **Stage 8 experimental coupled-reservoir model**. They do not replace the static Fill–Spill–Merge results, scenario caches, or HTTP endpoints. They do not add a rainfall-runoff transformation, overland-wave simulation, channel travel times, SWMM integration, or independently calibrated flood hydrographs.

The caller must explicitly provide a time profile. The API distributes each cell's **already calculated total effective runoff** over the storm intervals in the same proportions; dry D8 supplies the source-to-sink **spatial assignment**. All arrivals to a leaf basin are assumed to occur during those same intervals with **zero travel delay**. That is a declared simplifying assumption and can be highly unrealistic for large catchments.

## How to construct the inputs

1. Obtain the **original, unfilled and unbreached** DEM and corresponding `DepressionHierarchy`. The same analysis job and source grid must underlie each input. Map remains responsible for terrain acquisition.
2. Retrieve the Stage 7 `runoff_volume_m3` raster for a persisted rainfall scenario (or compute `calculateRainfallRunoff()` from its rainfall polygon). Each entry is a *total* event volume for one cell, in m³. `RainfallScenario::duration_seconds` is the event duration, not a rainfall intensity series.
3. Specify a **time profile**: positive interval durations in seconds and nonnegative fractions of the total effective-runoff volume. Durations must add to the scenario duration and fractions must add to one. A zero fraction is valid for a dry interval. The profile is **externally selected**, e.g. from a measured hyetograph; the adapter cannot infer it from DEM or total rainfall depth. A spatially uniform profile is applied to all cells, with constant effective-runoff fraction implicit in the Stage 7 volumes. The output is a prescribed **forcing**, not a calibrated hydrograph.
4. Prepare a `CoupledReservoir` for **each leaf** in the depression hierarchy, exactly once. `component_index` must equal that leaf's current `DepressionBasin::index`. Do **not** add parent merge-tree nodes alongside their children: parent storage already includes the children's terrain and would double-count the modeled volume. These internal indices are **not** long-term job UUIDs. Their use here is limited to a single consistent terrain analysis.
5. Supply each reservoir's **physically disjoint**, validated stage–storage curve, initial volume, and any calibrated hydraulic connections separately. The adapter does **not** derive these curves or their control coefficients. In particular, naive child storage curves may cease to represent independent hydraulic compartments above a shared saddle; the hydraulic model must handle the resulting shared water surface consistently before such a configuration is treated as a prediction.
6. Call the forcing builder and feed `basin_inflows` to `routeCoupledReservoirs()`. Do not feed the static redistribution's `overflow_m3`, `stored_in_subtree_m3`, or `basin_boundary_outflow_m3` into the dynamic solver as if they were additional runoff. Dynamic spill transfers must be computed by the hydraulic controls alone.

## Calculation and mass balance

Let `V_k` be the Stage 7 effective runoff from cell *k* (m³), and let `f_t` be the nonnegative fraction of total runoff assigned to time interval *t*. The original-DEM dry D8 flow graph directs each cell's runoff to either a **leaf depression minimum** or the **DEM boundary**. For a given leaf *i*, let `D_i` be the sum of the cell volumes that end there; let `B` be the direct runoff at the DEM boundary.

Then, for each time interval:

\[
I_{i,t}=f_t D_i,\qquad B_t=f_t B,\qquad \sum_t f_t=1.
\]

Here `I_{i,t}` is an **inflow volume** in m³ during the interval, not a discharge rate. The coupled solver internally assumes that volume arrives **uniformly during the interval**. To obtain the mean rate for reporting, divide by `duration_seconds`.

The adapter checks the source partition:

\[
\sum_k V_k=\sum_i D_i+B.
\]

It also verifies that the sum of its generated interval volumes equals the event total. Remainder handling assigns final-interval rounding differences back to their original source, instead of normalizing by modifying the total event rainfall.

**Domain boundary semantics:** `B` contains only cells whose *dry D8 path* reaches the DEM edge directly. It is recorded separately as `direct_boundary_outflow_m3` and `direct_boundary_outflow_m3_by_interval`; it is **not** injected into the closed reservoir model. This does not include **later basin overflow out of the domain**. The optional coupled boundary-outlet API can now simulate a separately characterized *free-overflow* reservoir exit (see [boundary outlets](BOUNDARY_OUTLETS.md)); it does not infer outlet geometry or flow timing from this adapter. The closed model's own mass balance checks `initial storage + injected basin inflow = final reservoir storage` within tolerance. For a closed example, the wider source balance also adds direct-boundary runoff. When combining the separate opt-in domain outlets, their transported volumes must be accounted for separately.

The DEM boundary is a numerical domain boundary, not proof of a physical river outlet. Incomplete upstream coverage or missing DEM elevations must not be treated as trustworthy runoff exits.

## Worked example

A 100-second event produces **200 m³** of effective surface runoff from a small catchment. Original-DEM dry routing delivers **150 m³** to its single depression leaf and **50 m³** directly to the DEM boundary. The explicit event profile specifies 25% of runoff over the first 20 s and 75% over the next 80 s.

| Period | Duration | Runoff share | Basin input | Direct DEM-edge runoff | Basin mean input rate |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 20 s | 25% | 37.5 m³ | 12.5 m³ | 1.875 m³/s |
| 2 | 80 s | 75% | 112.5 m³ | 37.5 m³ | 1.40625 m³/s |
| **Total** | **100 s** | **100%** | **150 m³** | **50 m³** | — |

Without any hydraulic connections and with sufficient stage–storage capacity, a reservoir initially empty finishes at 150 m³. Combined with the separately reported 50 m³ of dry D8 boundary runoff, this accounts for the full 200 m³ generated by Stage 7. This example does **not** model concentration time, backwater or stormwater network conveyance.

## C++ usage

```cpp
#include <aowis/hydrology/temporal_inflows.h>

using namespace Aowis::Hydrology;

// Existing same-source inputs:
// original_dem: ElevationGrid from Map
// basins: buildDepressionHierarchy(original_dem) or persisted hierarchy
// cell_runoff_m3: Stage 7 rainfall scenario's original DEM-aligned raster
// reservoirs: ONE CoupledReservoir for every basin leaf, each with its
//             component_index, disjoint storage curve and initial storage.

const std::vector<TemporalRainfallInterval> storm_profile = {
    {20.0, 0.25},
    {80.0, 0.75}
};
const TemporalRunoffForcing forcing = temporalInflowsFromRaster(
    original_dem, basins, cell_runoff_m3, reservoirs,
    storm_profile, 100.0);

CoupledRoutingOptions options;
options.maximum_substep_seconds = 1.0;
const CoupledRoutingResult simulation = routeCoupledReservoirs(
    reservoirs, calibrated_connections, forcing.basin_inflows, options);

// forcing.direct_boundary_outflow_m3 is separate from the closed solver.
// Check both forcing.mass_balance_error_m3 and the final
// simulation.snapshots.back().mass_balance_error_m3.
```

If the same spatial runoff will be simulated under several different storm profiles, compute `redistributeRunoff(original_dem, basins, cell_runoff_m3)` **once** and call `temporalInflowsFromRedistribution(basins, spatial_runoff, reservoirs, profile, duration)` repeatedly. Only `direct_inflow_m3` from leaf basins and `direct_boundary_outflow_m3` are consumed. The static solver's sibling filling, total stored water, and static basin-boundary overflow are **not** imported as time-dependent fluxes.

A rainfall scenario with **no depression leaves** can yield a valid forcing whose reservoir inflow arrays are empty: its runoff exits the DEM through dry D8. `routeCoupledReservoirs()` itself requires at least one reservoir, so there is no coupled reservoir simulation to run for that case.

## Validation and resource bounds

- Time profile: 1–10,000 intervals; every duration positive/finite, every fraction finite within [0,1], fractions summing to 1 within floating-point tolerance; total duration matches the scenario duration. A user must not silently reinterpret a rainfall total as a constant-intensity event without explicitly choosing that profile.
- Basin mapping: every hierarchy leaf represented by **exactly one** reservoir; no duplicate IDs; parent nodes refused; missing leaves refused. Columns of returned inflows follow the **input reservoir list order**, not the hierarchy sorting order.
- Spatial validation: original DEM-based dry D8 routing and the static result must match; invalid/negative/nonfinite direct inflow and unbalanced source totals are rejected. Input raster and hierarchy validation is delegated to `redistributeRunoff()` in the `FromRaster` entry point.
- Work limit: no more than 100,000 basin records, 100,000 coupled reservoirs, 10,000 intervals, and 5 million interval-by-reservoir output entries; both spatial routing and interval assembly allow cooperative cancellation. For many intervals and basins, even 5 million double entries can be substantial.
- The output represents **prescribed lateral volume inflows**. It does not supply calibration, observed flow rates, channel lag, pervious infiltration, travel routing, uncertainty estimates or sensor assimilation.

`tests/test_temporal_inflows.cpp` exercises full raster-to-reservoir accounting, time profile weighting, reversed leaf ordering, parent/duplicate rejection, all-boundary drainage, inconsistent source totals, missing inputs, cancellation and integration with a no-connection coupled run.

## Current and planned integration

**Implemented:** the independent C++ bridge in `AOWIS::Hydrology`, unchanged Map ownership of terrain, unchanged static rainfall/redistribution/inundation APIs, and an explicit distinction between dynamic reservoir inputs and direct DEM-edge runoff.

**Still not implemented:** native time-resolved rainfall rasters in Stage 7 scenario storage, field-calibrated travel-time routing and runoff attenuation, spatially variable hyetographs, automatic hydraulic-control survey/calibration, and integration with SWMM. **Implemented opt-in:** a separate coupled-simulation REST job can now load a saved event-total scenario using an explicit `scenario_forcing` time profile, retaining its **profile, assumptions, source checksum provenance and calibrated connection inputs** separately from static rainfall caches. See [Scenario-driven coupled forcing](SCENARIO_COUPLED_FORCING.md).

See [Rainfall scenarios](RAINFALL_SCENARIOS.md), [Runoff redistribution](RUNOFF_REDISTRIBUTION.md), [Coupled reservoir routing](COUPLED_RESERVOIR_ROUTING.md), and [Spill hydraulics](SPILL_HYDRAULICS.md) for the upstream calculation and hydraulic assumptions.

## Optional D8 travel-time adapter

The Qt-independent [`travelTimeInflowsFromRaster()`](TRAVEL_TIME_RUNOFF.md) now provides a **separate, opt-in** alternative that preserves the same destination partition but shifts each source-cell rainfall pulse by its accumulated original dry-D8 path distance divided by an explicitly supplied effective velocity. It can produce inflow intervals **after the rain has ended**, and retains direct boundary drainage in distinct per-interval fields. The zero-delay `temporalInflowsFromRaster()` and `temporalInflowsFromRedistribution()` APIs documented here are unchanged. Both methods are simplified forcing models, not field-calibrated runoff hydrographs. See the [travel-time equations, example and performance limits](TRAVEL_TIME_RUNOFF.md).

## Coupling with reservoir-to-domain outflow

The separate [boundary outlet API](BOUNDARY_OUTLETS.md) now supports one-way hydraulic discharge from a modeled reservoir to the exterior. **This is not the same water as the direct dry-D8 boundary runoff** already recorded by this forcing adapter. Keep these contributions separate: `direct_boundary_outflow_m3` (spatially bypasses every modeled basin) and `CoupledRoutingSnapshot::cumulative_boundary_outflow_m3` (leaves *after entering* a modeled reservoir). Do not inject the adapter's direct outflow into any reservoir or recount it as a hydraulic outlet transfer. This older adapter still has zero delay; the separate [travel-time adapter](TRAVEL_TIME_RUNOFF.md) models an assumed dry-D8 lag for this direct-boundary runoff as well.
