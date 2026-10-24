# Coupled reservoir routing (C++ core)

## Purpose and present status

`routeCoupledReservoirs()` solves a **time-dependent, zero-dimensional finite-volume reservoir network**. It is an opt-in core API. It uses independently specified basin stage–storage curves, externally parameterized hydraulic connections, and per-reservoir, time-interval inflow volumes. Every connection is evaluated from the **same instantaneous set of water levels** and all calculated transfers are applied simultaneously.

This is a foundation for timed, hydraulically informed flow redistribution. It is **not** the same as the existing static Fill–Spill–Merge rainfall redistributor, and it does not silently replace any saved scenarios, redistribution caches, inundation files, or REST endpoints. A caller has to construct and validate a reservoir network and its inflow history explicitly.

The implementation deliberately does **not** manufacture any of the following from Copernicus GLO-30: measured spillway width, discharge coefficient, submergence curve, engineered opening dimensions, sub-hourly rainfall history, per-basin hydrograph, or a reliable street-level drainage network.

## Model and units

Each reservoir `i` has a monotone stage–storage relationship `S_i(h)`:

- `h`: standing-water surface elevation, metres, in one common vertical datum;
- `S`: stored volume, m³;
- `Q_ij`: signed discharge from reservoir `i` to `j`, m³/s;
- `I_i(t)`: externally specified, nonnegative reservoir inflow, m³/s.

For each hydraulic step of length `dt`, the original closed-network solver approximates

\[
S_i(t+dt)=S_i(t)+I_i(t)dt+\sum_{j}Q_{ji}(t)dt-\sum_j Q_{ij}(t)dt.
\]

The open-boundary variant additionally subtracts each reservoir's outlet discharge `Q_boundary,i(t) dt` from its storage and adds the identical volume to cumulative exterior discharge. It evaluates the discharge at the **start** of the substep (forward Euler). A discharge on an edge is subtracted from its sender and added to its receiver **exactly once**. Consequently, transfers do not create or destroy water. Across the original closed network (or the opt-in open network after adding cumulative outlet discharge to the storage side):

\[
\sum_i S_i(t) + V_{\mathrm{boundary}}(t)
= \sum_i S_i(0)+\sum_{k\text{ elapsed}}\sum_i V_{i,k}^{\mathrm{inflow}}.
\]

The result checks this invariant at every user-supplied interval boundary. `mass_balance_error_m3` is the computed storage **plus cumulative modeled boundary outflow** minus the starting storage and injected volume (the outflow term is zero for the original closed-network API). The allowed round-off residual is `1e-9 × max(1 m³, initial total volume, cumulative inflow, cumulative boundary outflow)`; **this tolerance is a numerical acceptance threshold, not a hydrological accuracy guarantee**.

### Stage–storage curves

The caller supplies `SpillReservoirCurve::points`, sorted by *strictly increasing* water levels and volumes. The first point must have zero volume. Between knots, storage and stage are linearly interpolated. Extrapolation beyond the supplied curves is **not performed**. Curves must represent **nonoverlapping hydraulic control volumes**: do not simultaneously add a depression's parent storage and its descendant storage, because those share terrain volume. The existing depression hierarchy's nested storage counts are not automatically valid independent reservoir curves at all levels.

### Connections and physical data

The current routing core supports two explicitly configured connection types:

| Connection type | Uses | Unsupported/required information |
|---|---|---|
| `CalibratedBroadCrestedWeir` | Existing free-flow weir formula plus user-supplied tailwater reduction curve | Effective hydraulic width, crest, loss factor and calibration knots; does not infer these from raster face contacts |
| `FullySubmergedRectangularOrifice` | `Q = Cd × aperture area × sqrt(2 g × abs(head difference))` with direction from higher to lower stage | Requires survey/engineering dimensions, invert, coefficient; partially submerged openings are *not* rated |

Each control includes a `BasinSpillJunction`, and its component IDs must correspond to two **cardinally contacting** basin components for a broad-crested weir (two valid components for an orifice). The junction is DEM topology, **not** a calibrated control section. The map from hydraulic reservoir IDs to below-saddle depression components is the caller's responsibility. Avoid placing a set of independent controls across the same physical spill path unless it genuinely has separate discharge capacity.

Free discharge transitions into calibrated submergence when both reservoir surfaces are above the weir crest; the correction ratio is downstream head divided by upstream head. The rating is externally supplied and interpolated. See [spill hydraulics](SPILL_HYDRAULICS.md) and [submerged controls](SUBMERGED_SPILL_CONTROLS.md) for equations and parameter validation.

## Timestep and simultaneous coupling

The caller supplies a sequence of `CoupledInflowInterval { duration_seconds, inflow_volume_m3[] }`. Each vector's entries correspond to the **input reservoir vector order**. Input volume is applied uniformly over the given interval, and the entire interval is subdivided into pieces no longer than `maximum_substep_seconds`.

For each candidate substep:

1. Compute all basin levels by interpolating their current storage.
2. Evaluate every connection's signed discharge using those *same* levels.
3. Apply each external inflow and every connection's signed transfer to proposed reservoir volumes (without changing the actual state yet).
4. **Equalization limit:** cap each connection's substep volume at the volume that would equalize the two stages it connects, given all other simultaneous changes. The exact stage ODE approaches but never crosses equal stages, so this bound is physical. All caps are evaluated against the same proposed state and applied together (order independent); a few passes settle connections sharing a reservoir. Every capped transfer is counted in `equalization_limited_transfers`.
5. Reject the entire candidate and halve `dt` if any reservoir would leave its stage–storage curve, a connection's *own* transfer would reverse its head gradient, a weir transfer would push its donor below the crest, or an active orifice transfer would uncover its crown. A reversal caused by inflow into the receiver (or by other controls) is physical and is accepted.
6. Only after all checks pass, commit all reservoir volumes and cumulative transferred volumes together.

Without step 4, an orifice (`Q ~ sqrt(head)`) needs explicit substeps proportional to `sqrt(head)` near equalization, so the substep budget was exhausted as soon as two connected reservoirs approached a common level (about 21 s in the regression fixture of `tests/test_coupled_routing.cpp`). Weir crests, outlet crests and curve limits are deliberately **not** clipped; they still reject and subdivide the substep.

This avoids index-order preference and detects numerically excessive timesteps. A large `equalization_limited_transfers` count means the substep is coarse relative to the equalization time scale; refine it before interpreting stage histories. However, it remains explicit Euler, not an implicit solution of the network: **halve `maximum_substep_seconds` and compare results** before interpreting time-dependent flood levels. A trial that remains unstable after repeated refinements, or exceeds its total accepted/rejected substep budget, fails rather than extrapolating.

Unsupported physical regimes (for example, a partially submerged orifice with a water-level difference) cause an explicit exception; reducing the timestep does not make an unknown discharge law acceptable. The original `routeCoupledReservoirs()` call remains a **closed** reservoir network. The opt-in `routeCoupledReservoirsWithOutlets()` separately supports validated, **one-way free-overflow outlets** to the exterior. Each outlet uses an independently supplied crest, width, factor and permanently sub-crest downstream level. It reports cumulative boundary discharge and tests `stored + boundary discharge = initial + prescribed inflow`. It does **not** support submerged outlets or reverse external inflow. See [free-overflow domain boundaries](BOUNDARY_OUTLETS.md). If a reservoir exceeds the provided curve, the solver reports an error rather than losing the excess water.

The calculation is bounded: at most 100,000 reservoirs, 200,000 connections, 100,000 input intervals, a configured maximum output-interval count, and a configured maximum total number of accepted/rejected substeps (up to 100 million). Cancellation is checked during routing and produces a `std::runtime_error`.

### Optional travel-delayed rainfall forcing

Use the [travel-time-aware forcing adapter](TRAVEL_TIME_RUNOFF.md) to produce `CoupledInflowInterval` values whose volumes follow an explicitly assumed **dry-D8 path velocity**. That adapter retains a full post-rainfall arrival tail and keeps direct DEM-edge runoff separate from modeled hydraulic outlet discharge. It does not calibrate a velocity or produce time-of-arrival observations; the original immediate-delivery [temporal forcing](TEMPORAL_RUNOFF_FORCING.md) remains an independent alternative.

## C++ usage: two connected reservoirs

```cpp
#include <aowis/hydrology/coupled_routing.h>

using namespace Aowis::Hydrology;

// Two independently represented pond volumes. These are synthetic curves;
// supply measured or validated curves in applications.
const SpillReservoirCurve curve{{{0.0, 0.0}, {10.0, 1000.0}, {20.0, 2000.0}}};
const std::vector<CoupledReservoir> reservoirs = {
    {10, curve, 1200.0},
    {20, curve, 800.0}
};

// junction must be a verified two-component BasinSpillJunction obtained from
// extractSpillGeometry(). This example assumes it exists in the caller.
CoupledConnection connection;
connection.type = CoupledControlType::CalibratedBroadCrestedWeir;
connection.junction = junction;
connection.weir.first_component_index = 10;
connection.weir.second_component_index = 20;
connection.weir.crest_elevation_m = 10.0;
connection.weir.effective_width_m = 2.0; // external hydraulic parameter
connection.weir.discharge_factor = 0.9; // externally calibrated
connection.submergence.points = {{0.0, 1.0}, {0.5, 0.8}, {1.0, 0.0}};

CoupledRoutingOptions options;
options.maximum_substep_seconds = 0.1;
const std::vector<CoupledInflowInterval> intervals = {
    {60.0, {50.0, 25.0}},
    {60.0, {0.0, 0.0}}
};
const CoupledRoutingResult result = routeCoupledReservoirs(
    reservoirs, {connection}, intervals, options);

// snapshots[0]: initial levels and storage.
// snapshots[1]: after 60 s; snapshots[2]: after 120 s.
// snapshot.connection_transfer_m3[0]: signed cumulative transfer 10 -> 20.
// snapshot.mass_balance_error_m3: should remain near zero.
```

**Important:** The sample's `junction` is a prerequisite, not a fabricated terrain measurement. The numerical calibration points are illustrative only. For a production run, tie IDs to the actual two contacting components and replace the sample curves and hydraulic inputs with defensible data.

## Worked mass-balance example

Suppose the reservoirs initially contain 1,200 m³ and 800 m³; during a 60-second interval, 50 m³ and 25 m³ of effective runoff enter them. If the control transfers 30 m³ from the first reservoir to the second, the resulting volumes are:

| Reservoir | Initial | Inflow | Net transfer | Final |
|---|---:|---:|---:|---:|
| 10 | 1,200 | +50 | −30 | 1,220 m³ |
| 20 | 800 | +25 | +30 | 855 m³ |
| **Total** | **2,000** | **+75** | **0** | **2,075 m³** |

The **30 m³ is illustrative**, not the predicted transfer from the sample parameters above. Actual transfer depends on the head and submergence response over the full interval. The solver reports actual sampled end-of-interval storages and cumulative transfers.

## Output semantics

`CoupledRoutingResult.snapshots` contains the initial state and one snapshot per input interval. Each snapshot records elapsed time (s), each reservoir's volume (m³) and water level (m), each connection's **cumulative signed** transferred volume (m³), cumulative external inflow (m³), total retained storage (m³), and total mass-balance residual (m³). The `accepted_substeps`, `rejected_substeps` and `equalization_limited_transfers` counters help assess stiffness and numerical cost.

The reported water levels belong to **lumped reservoir approximations**. A snapshot is not a cellwise flood-depth raster, does not model sheet flow across the DEM, and does not predict street-level inundation or arrival times in the surrounding landscape.

## Integration with the existing AOWIS services

- **Map** continues to own DEM acquisition and cached source tiles. It does not own this hydraulic solver.
- **Hydrology** already produces static basin topology, storage relationships, and total runoff volumes. This new core is deliberately separate from scenario persistence and REST: event-duration and total rainfall depth alone do **not** determine an inflow hydrograph.
- The opt-in [timed-runoff forcing adapter](TEMPORAL_RUNOFF_FORCING.md) now distributes saved scenario event runoff totals into **externally specified time fractions** and maps the original DEM's dry-D8 leaf inflow volumes into per-reservoir intervals. This is not a travel-time calculation: all dry-D8 arrivals are assumed immediate, and the caller must separately provide physically disjoint reservoir curves and calibrated controls.
- The opt-in coupled simulation service now has separate job UUIDs, explicit input manifests, versioned cache files, cancellation and status APIs. It never overwrites static redistribution results or reuses their cache keys. It does not yet automatically build inflow histories from saved rainfall scenarios.
- The opt-in [free-overflow boundary outlet](BOUNDARY_OUTLETS.md) now accounts for discharge leaving modeled reservoirs while preserving simultaneous update and per-snapshot conservation. It is not an inferred DEM feature or backwater calculation; it can now be configured explicitly through the separate persisted coupled simulation service. Physically defensible time-varying infiltration and connections to SWMM remain future capabilities.

See [numerical verification and reference cases](NUMERICAL_VERIFICATION.md) for
closed-form and independently integrated routing benchmarks, observed
first-order timestep convergence, discharge-error measurements, and the
limits of conservation-only assertions.

## Validation and known limitations

`tests/test_coupled_routing.cpp` checks signed two-reservoir transfer, external inflows across successive intervals, conservation, simultaneous three-reservoir coupling, connection-list order invariance, step rejection, submerged-orifice cases, bad input, cancellation and work-limit exhaustion. All other hydrology tests remain present. The model must also be compared against independently validated reservoir-routing solutions before operational use.

The solver **does not** yet have:

- physically inferred rainfall-to-runoff timing or calibrated travel-time routing; the immediate adapter applies explicit event-time fractions with zero delay, while the [separate D8 adapter](TRAVEL_TIME_RUNOFF.md) shifts them by a caller-assumed constant effective velocity;
- a **submerged or reversible** outside-domain boundary, floodplain surface propagation, infiltration, evapotranspiration or negative discharge boundaries (the new opt-in terminal outlet handles one-way **free-overflow** only);
- implicit time integration / hydraulic inertia / momentum conservation;
- general culvert hydraulics, partially submerged apertures, complex multiway controls, backwater through connected channels or surveyed spillway cross-sections sourced automatically;
- calibration against measured hydrographs or terrain reference tools.

**Do not interpret mass conservation as hydraulic calibration.** The present result is a closed-network lumped-parameter numerical simulation conditional on supplied storage curves, control ratings, initial states, and inflow history.

## Service execution

A separate [coupled simulation service API](COUPLED_SIMULATION_SERVICE.md) now supports asynchronous execution of independently configured hydraulic reservoir networks, atomic time-series persistence, and cache-only paginated REST reads. It requires explicit reservoir curves, calibrated controls and per-interval inflows; it does not change existing rainfall scenarios or the static redistribution and inundation results.
