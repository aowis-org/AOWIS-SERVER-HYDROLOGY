# Free-overflow domain boundaries for coupled reservoir routing

## Purpose and availability

The Qt-independent coupled reservoir core can route **water leaving the modeled reservoir network** through explicitly characterized outlet structures. This complements the existing simultaneous basin-to-basin transfers and externally prescribed rainfall runoff. Boundary discharge is accounted for at every accepted numerical substep and reported separately from internal hydraulic transfers.

This is an **opt-in experimental C++ API**, not a change to the production static runoff-redistribution algorithm. No REST endpoint, persisted rainfall result, inundation cache or Map-server interface changes as a consequence. The existing `routeCoupledReservoirs()` entry point retains its closed-network behavior; `routeCoupledReservoirsWithOutlets()` enables open boundaries.

## Physical interpretation and required inputs

Each `CoupledBoundaryOutlet` is a **one-way rectangular broad-crested free-overflow control** that drains one modeled reservoir into the exterior. Its parameters must be supplied from field survey, reliable engineering information, or explicit calibration:

| Field | Units | Meaning |
|---|---|---|
| `reservoir_component_index` | ID | Existing independent reservoir, not the hierarchy parent and child simultaneously |
| `crest_elevation_m` | m | Physical control crest in the **same vertical datum** as reservoir stages |
| `effective_width_m` | m | Hydraulically effective width, not the length of raster contact faces |
| `discharge_factor` | dimensionless | Calibrated multiplier on the ideal rectangular critical-flow law, `0 < factor <= 1` |
| `downstream_water_level_m` | m | Assumed constant downstream water level; must remain **strictly below** the crest |

Only the upstream reservoir's volume and stage are modeled. The exterior does **not** have a guessed reservoir curve or dynamically changing stage. Downstream tailwater enters this version only as a **free-flow applicability condition**, not as a head-loss term. If the downstream level rises to/above the crest, or exterior water could flow back into the basin, this boundary type is inappropriate and is rejected at configuration time. There is no hidden reverse-flow or submerged-flow approximation.

The free-overflow equation is consistent with the existing broad-crested control model:

\[
H = \max(0, z_{\mathrm{upstream}}-z_{\mathrm{crest}})
\]

\[
Q = C_d b \sqrt{g}\left(\frac{2H}{3}\right)^{3/2}
\]

where `Q` is m³/s, `C_d` is the dimensionless discharge factor, `b` is effective width in m, and `g = 9.80665 m/s²`. `Q` is **always nonnegative** and is zero when the upstream level is at or below the crest. The solver evaluates `Q` at the beginning of each accepted substep; this is first-order explicit Euler integration, **not** a momentum-equation solution.

A terrain-boundary pixel, corner-only saddle, or raster contact length **does not** establish a surveyed physical outlet. The host model is responsible for specifying which hydraulic control actually discharges from the domain and ensuring that separate configured outlets are not duplicate descriptions of the same physical opening.

## C++ usage

```cpp
#include <aowis/hydrology/coupled_routing.h>

using namespace Aowis::Hydrology;

CoupledReservoir pond;
pond.component_index = 10;
pond.curve.points = {{0.0, 0.0}, {10.0, 1000.0}, {20.0, 2000.0}};
pond.initial_storage_m3 = 1200.0;

CoupledBoundaryOutlet outlet;
outlet.reservoir_component_index = 10;
outlet.crest_elevation_m = 10.0;
outlet.effective_width_m = 2.0;
outlet.discharge_factor = 0.9;
outlet.downstream_water_level_m = 8.0;

CoupledRoutingOptions options;
options.maximum_substep_seconds = 0.1;
const CoupledRoutingResult result = routeCoupledReservoirsWithOutlets(
    {pond}, {}, {outlet}, {{10.0, {50.0}}, {10.0, {0.0}}}, options);

const CoupledRoutingSnapshot &end = result.snapshots.back();
// end.total_storage_m3: water still retained in modeled reservoirs
// end.boundary_outflow_m3[0]: cumulative m³ discharged by this outlet
// end.cumulative_boundary_outflow_m3: cumulative m³ leaving via ALL outlets
// end.cumulative_inflow_m3: prescribed additional inflow into reservoirs
// end.mass_balance_error_m3: numerical residual of the conservation equation
```

The reservoir stage–storage curve must be continuous across the control crest and have sufficient range for all calculated water levels. The curve above is **synthetic test data**, not a representation of Copernicus terrain. Likewise, example crest/width/coefficient values are illustrative and require replacement with actual source information for a real site.

### One control vs. many

The caller may provide multiple reservoirs, internal `CoupledConnection`s and multiple boundary outlets in one run. Every outlet and internal connection is rated at the **same start-of-substep reservoir water levels**. The proposed individual changes are accumulated before checking feasibility and accepting the complete simultaneous update. This prevents a boundary outlet from taking priority over another outlet solely because of its position in the input vector.

`boundary_outflow_m3` is indexed in the **caller-supplied outlet order**. It is cumulative since the start of the simulation, not a per-interval delta or a discharge rate. To obtain the volume over one reporting interval, subtract adjacent snapshots. Divide by interval duration for an interval-average discharge. `connection_transfer_m3` remains signed and internal and must **not** be added to the global domain outflow.

## Water balance and worked example

For a system of `N` independent reservoir storage volumes and `M` terminal outlets, the overall budget at any snapshot is:

\[
\sum_{i=1}^{N}S_i(t) + \sum_{o=1}^{M}V_{\mathrm{out},o}(t)
= \sum_{i=1}^{N}S_i(0) + V_{\mathrm{in}}(t)
\]

For example, assume 1,200 m³ is initially stored, then an interval adds 50 m³ of runoff, and the external outlet discharges 30 m³. The resulting retained water is 1,220 m³ and the check is `1,220 + 30 = 1,200 + 50 = 1,250 m³`. The **30 m³ is illustrative**, not the rate predicted for the example parameters: actual flow depends on the stage history integrated over the interval.

The snapshot reports:

- `total_storage_m3`: sum of independent modeled reservoir storage volumes (m³).
- `cumulative_inflow_m3`: sum of prescribed inflow volumes across all reservoirs and elapsed intervals (m³).
- `boundary_outflow_m3`: cumulative discharged volume for each outlet (m³).
- `cumulative_boundary_outflow_m3`: sum of cumulative outlet discharge volumes (m³).
- `mass_balance_error_m3`: computed `storage + outlet_outflow - initial_storage - inflow` (m³); near zero to numeric tolerance.

The original, closed-network call returns **empty** `boundary_outflow_m3` and **zero** `cumulative_boundary_outflow_m3`. These added snapshot fields do not change the meaning of any existing fields.

### Relationship to timed runoff forcing

The existing [temporal forcing adapter](TEMPORAL_RUNOFF_FORCING.md) separately reports **direct runoff that reaches the analysis-domain boundary** by dry D8 paths without first entering a modeled basin. That volume **must not** be passed through a boundary outlet as though it were stored inside a reservoir. When combining the two subsystems, count *direct dry-D8 boundary discharge* and *coupled reservoir outlet discharge* as distinct outflow contributions. The adapter still has zero source-to-basin travel delay and still requires an explicit event-time profile.

## Stability, limits, and errors

A candidate substep is rejected and halved if its simultaneous set of inflows/transfers/outflows would deplete a reservoir below zero, exceed its stage–storage curve, reverse an active interior head gradient, or lower a reservoir beneath an active outlet crest. This is a timestep *rejection*, not an unconstrained, silent clipping of outlet volume.

A very high crest discharge coefficient, very narrow stage–storage interval, or a large prescribed inflow can require smaller time steps. Compare results at two different `maximum_substep_seconds` values; first-order explicit integration is only approximate. The existing limits on substep count, cancellation, reservoir count and connection count remain. At most **200,000 combined internal connections and boundary outlets** are allowed; the caller is responsible for memory/CPU budgeting and correct physical configuration.

The API rejects nonfinite dimensions/levels/factors, zero or negative widths, factors outside `(0,1]`, unknown reservoir IDs, downstream stages at/above crest, or a crest outside the usable stage–storage curve. It throws on unsupported or numerically infeasible conditions rather than publishing a plausible but invalid flood prediction.

## Explicitly not implemented

- Dynamic downstream water-level hydrographs, tidal boundaries, submerged tailwater ratings, reversible boundary backflow or flood gates.
- Engineered partial-opening outlets, downstream pipe/friction losses, general culvert equations, multiway contact hydraulics or energy/momentum-based surface flow.
- Automatic discovery of physical outlet width and coefficient from Copernicus GLO-30 pixels.
- Geographical flood arrival times, street-level depth maps from these lumped time-dependent reservoir snapshots, calibration against observed floods, or a general-purpose SWMM replacement.
- A persisted time-dependent simulation service, asynchronous HTTP job or database representation for outlet parameters.

For multi-basin context, see [coupled reservoir routing](COUPLED_RESERVOIR_ROUTING.md). For the existing calibrated **internal** weir/orifice primitives, see [submerged spill controls](SUBMERGED_SPILL_CONTROLS.md). The present outlet law is intentionally narrower: it permits reliable one-way **free overflow** without inventing downstream hydraulics.

## Verification

`tests/test_boundary_outlets.cpp` checks the analytical discharge law, dry outlets, long-event crest protection with adaptive rejection, prescribed inflows, multiple simultaneously rated outlets and input-order invariance, interior weir plus boundary flow, conservation at each output interval, repeated synthetic scenarios, parameter validation, cancellation, and legacy API compatibility. It is registered as `aowis-hydrology-boundary-outlets` under the `hydrology;aowis` CTest labels.

Build and run the normal suite with `./compile_linux_tests.sh --both`. Successful synthetic tests establish consistency with the stated equations and invariants, not independent field validation of the chosen controls.
