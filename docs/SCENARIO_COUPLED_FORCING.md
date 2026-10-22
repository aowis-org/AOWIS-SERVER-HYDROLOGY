# Saved rainfall scenarios as coupled-simulation forcing

## Purpose and boundaries

A completed Hydrology terrain-analysis job can store several **Stage 7 rainfall scenarios**. Each scenario contains the total effective runoff volume per raster cell (`runoff.f64`) and an event duration. The coupled reservoir solver accepts a time series of per-reservoir inflow volumes. The **scenario-forcing adapter** connects these two subsystems without modifying the rainfall scenario, the static redistribution cache, or existing simulations.

This is an **opt-in experimental screening model**. A scenario duration and total depth do **not** determine rainfall timing, infiltration dynamics, catchment velocities, culvert dimensions, stage–storage curves or physically calibrated discharge. Supply the missing timing and hydraulic parameters explicitly. No connection widths are inferred from 30 m DEM pixels.

The same `AnalysisService::startCoupledSimulation()` call works in the in-process library and through HTTP. Existing calls that supply an explicit `inflows` array continue unchanged. Supply **exactly one** of `inflows` and `scenario_forcing`.

## Example REST request

Submit to `POST /v1/jobs/{analysis-uuid}/coupled-simulations`:

```json
{
  "reservoirs": [
    {
      "component_index": 0,
      "initial_storage_m3": 0,
      "stage_storage": [[10, 0], [15, 4500], [20, 9000]]
    }
  ],
  "connections": [],
  "outlets": [],
  "scenario_forcing": {
    "scenario_id": "00000000-0000-4000-8000-000000000001",
    "delivery_model": "d8_constant_velocity",
    "time_profile": [
      {"duration_seconds": 240, "runoff_fraction": 0.25},
      {"duration_seconds": 360, "runoff_fraction": 0.75}
    ],
    "effective_velocity_m_per_s": 0.5,
    "output_interval_seconds": 60
  },
  "options": {"maximum_substep_seconds": 1, "maximum_substeps": 1000000}
}
```

Replace the UUID with a real scenario ID **from the same analysis job**, and substitute actual component indices, stages and calibrated physical inputs for these illustrative values. In this example, the stage–storage curve covers a single independent leaf depression; a connected multi-basin system requires distinct non-overlapping control volumes and surveyed/calibrated internal connections.

The options `reservoirs`, `connections`, `outlets`, and the coupled solver's `options` are otherwise identical to [Coupled Simulation Service](COUPLED_SIMULATION_SERVICE.md). HTTP returns the simulation UUID immediately; poll the existing status endpoint and read bounded snapshot pages as before.

### Forcing variants

| `delivery_model` | Runoff arrival | Additional parameters | Meaning |
|---|---|---|---|
| `instant` | Time profile is applied immediately to each basin's dry-D8 contributing area | none | No source-to-basin travel delay |
| `d8_constant_velocity` | Each raster cell's time profile is shifted by dry-D8 distance / specified velocity | `effective_velocity_m_per_s`, `output_interval_seconds` | Constant assumed effective routing speed, **not** modeled channel hydraulics |

`time_profile` is a nonempty list of `{duration_seconds, runoff_fraction}`. Durations must be positive and sum to the scenario's **saved** `duration_seconds`. Runoff fractions must be nonnegative and total 1.0 (small numerical rounding is tolerated by the hydrology core). For `instant`, omit the velocity and output-interval parameters entirely. For `d8_constant_velocity`, supply both as positive finite numbers. The delayed model may generate **additional post-rainfall intervals**, to deliver the tail that arrives after the rain stops.

One coupled reservoir must be supplied for **every leaf depression**, using that leaf's **internal `component_index`** from the saved hierarchy. The reservoir list defines the column order of inflow arrays. **Do not** supply parent and child basins together: their stored volumes overlap. The service validates that exact leaf mapping through the underlying forcing adapter and rejects missing, duplicate and parent-only assignments.

The C++ core supports larger workloads than the service; the service caps simulation reservoirs at 64, connections at 128, outlets at 64, and **generated time intervals at 1,000** (including delayed tail bins). Use an appropriate output bin size for the region, event duration, and travel delays. Rejected requests or worker failures do not silently truncate runoff volume.

## Processing and mass accounting

1. Confirm the analysis job is complete, the scenario manifest is valid, and its ID matches the requested scenario.
2. Read the **original**, unfilled `elevation.f32` DEM, the saved `basins.aowbin` depression hierarchy, and the saved cellwise `runoff.f64` volumes. These files, rather than the potentially conditioned D8 analysis surface, determine dry-flow destinations.
3. Route every runoff cell to the first depression leaf or direct DEM boundary. The routing adapter cross-checks the resulting destination totals against the static redistribution's pre-spill leaf inputs. It does **not** import static overflow/storage allocations as dynamic inflows.
4. Allocate those event-total volumes according to the supplied time profile. With travel-time routing, shift each cell's pulses by its computed path delay before accumulating time bins.
5. Run the coupled reservoir solver with these generated per-leaf inflow intervals and the independently specified control sections. Hydraulic domain-outlet volumes are computed by the solver, not by the runoff adapter.
6. Persist the simulation's results and **source checksums / assumptions** in the simulation manifest before publishing it as completed.

For an event with total generated effective runoff `R`:

\[
R = B + D
\]

where `B` is the total runoff delivered to **all modeled basin reservoirs** and `D` is direct dry-D8 drainage outside the DEM. Within the coupled network:

\[
S_{\mathrm{initial}}+B = S_{\mathrm{final}}+O
\]

where `O` is the sum of discharge through **explicitly configured hydraulic boundary outlets**. It follows that:

\[
S_{\mathrm{initial}}+R = S_{\mathrm{final}}+O+D.
\]

**Do not** add `D` to basin inflows or to the coupled solver's `cumulative_boundary_outflow_m3`; `D` is already outside that reservoir network. The solver's snapshot `mass_balance_error_m3` checks its **internal** balance, while the scenario adapter reports an independent forcing balance.

### Worked volume example

Suppose a scenario creates 200 m³ of effective runoff. Dry D8 sends 150 m³ to a single depression and 50 m³ directly outside the raster. A two-interval profile carries 25% and 75% of runoff respectively:

| Interval | Basin inflow | Direct DEM-boundary runoff |
|---|---:|---:|
| First interval | 37.5 m³ | 12.5 m³ |
| Second interval | 112.5 m³ | 37.5 m³ |
| **Total** | **150 m³** | **50 m³** |

Now suppose the reservoir starts with 20 m³, retains 130 m³ at event end, and discharges 40 m³ through an explicitly modeled outlet. The network balance is `20 + 150 = 130 + 40`. Adding the independent dry-boundary volume gives `20 + 200 = 130 + 40 + 50`. Travel delays change **when** those volumes arrive, not their totals.

## Result provenance and integrity

Status and snapshot responses include a `scenario_forcing_result` object for scenario-driven simulations. It is absent/null for independently prescribed inflows. Fields include:

- `scenario_id`, `delivery_model`, `event_duration_seconds`, `output_duration_seconds`, `interval_count`
- `maximum_source_travel_seconds` (zero for instantaneous forcing)
- `runoff_generated_m3`, `delivered_to_basins_m3`, `direct_dem_boundary_runoff_m3`, `mass_balance_error_m3`
- `timing_assumed: true` and, where used, `effective_velocity_m_per_s` and `output_interval_seconds`
- `source_sha256`: SHA-256 hashes for the scenario JSON manifest, runoff raster, original DEM, and basin-hierarchy artifact

The service retains the **exact submitted JSON request** in `simulation.json`. The separate `scenario_forcing_result` is written when the worker finishes generating valid input; it is included in the completed manifest and returned after server restart. The existing `series-v1.aowbin` embeds a SHA-256 digest of that immutable submitted request and checksum-verifies each requested snapshot row. A changed source file does not silently regenerate or modify an existing simulation: to rerun against updated terrain/runoff, submit a new simulation. The recorded hashes allow the input revision used for the run to be distinguished. No persistent time-binned forcing raster is currently exposed.

**Failure modes:** nonexistent scenario, modified/truncated raster, mismatched manifest total, invalid leaf mapping, timing-profile disagreement, excessive post-event bin count, unsupported hydraulic regime, invalid stage curve, or canceled task result in a failed/canceled simulation. A queued task interrupted by server restart remains `interrupted`; it is not automatically re-simulated.

## Interpretation and limitations

- This is a lumped reservoir routing model, **not** a 2D shallow-water, surface-roughness, or SWMM hydraulic solver.
- The caller must supply a **runoff timing profile**. Uniform effective-runoff fractions over the region do not represent spatially variable rainfall or time-varying infiltration.
- The D8 velocity is a **single assumed speed** across hillslopes/channels. It does not account for momentum, roughness, slope-based kinematic waves, interactions with changing pond levels, or flooded-path rerouting.
- Hydrologic destinations use the saved original DEM, with all uncertainty from source resolution, missing culverts and artificial DSM barriers. Input DEM coverage must be sufficient for the full upstream basin.
- Explicit curves and hydraulic control widths, coefficients, ratings and vertical datum must be self-consistent and justified independently of GLO-30.
- `d8_constant_velocity` yields delayed basin inflow only; it does not calculate water arrival or depth at every raster cell. Dynamic flood-depth maps require separate defensible hydraulics.

## Tests and operational checklist

`tests/test_service.cpp` exercises both forcing variants on a saved single-depression scenario, a consistent runoff-to-basin balance, malformed/ambiguous input rejection, scenario source digests, status, and paginated results. The Qt-independent [temporal](TEMPORAL_RUNOFF_FORCING.md) and [D8 travel-time](TRAVEL_TIME_RUNOFF.md) test suites verify conservation, distances, tails, cancellation, and leaf mapping independently.

To validate a real project, first run `./compile_linux_tests.sh --both`; then submit a small scenario and inspect `scenario_forcing_result`, compare basin inflow totals against the saved Stage 7 runoff, inspect delayed tail bins, and confirm hydraulic outflow/basin storage balance. Finally, rerun at half `maximum_substep_seconds` and with alternative physically plausible velocities/profile shapes to assess sensitivity. **Conservative volume balance and passing tests do not demonstrate calibrated flood prediction.**
