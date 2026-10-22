# Coupled reservoir simulation service

## Purpose and modeling boundary

The Hydrology server supports two **independent** families of results:

1. **Static rainfall redistribution / standing-water inundation:** terrain-driven Fill–Spill–Merge estimates based on the persisted original DEM and Stage 7 event-total runoff. These have their own scenario UUID, binary cache and REST lifecycle.
2. **Time-dependent coupled reservoir simulations:** a prescribed, lumped-reservoir hydraulic model driven by **explicit or scenario-derived per-reservoir inflow intervals** and **externally characterized** storage curves and control sections. Each run has a **separate simulation UUID** under an already completed terrain-analysis job. It uses `routeCoupledReservoirsWithOutlets()` and never changes the static result or assumes rain-depth and duration constitute a hydrograph.

The client is responsible for defensible stage–storage models and calibrated controls. An inflow history may be supplied directly through `inflows`, or generated from a saved Stage 7 scenario using an explicit `scenario_forcing` configuration. The latter uses the [temporal](TEMPORAL_RUNOFF_FORCING.md) or [travel-time](TRAVEL_TIME_RUNOFF.md) core adapter but **never infers rainfall timing, travel speed, or a calibrated hydrograph from event-total rainfall**. See [Scenario-driven forcing](SCENARIO_COUPLED_FORCING.md) for the full schema, accounting, provenance, and assumptions.

## REST lifecycle

| Method | Endpoint | Behavior |
|---|---|---|
| `POST` | `/v1/jobs/{job-id}/coupled-simulations` | Validate basic request, enqueue independent simulation; HTTP 202 |
| `GET` | `/v1/jobs/{job-id}/coupled-simulations/{simulation-id}` | Read status, no solver work |
| `GET` | `/v1/jobs/{job-id}/coupled-simulations/{simulation-id}/snapshots?offset=0&limit=25` | Read verified, bounded time-series rows, no solver work |
| `DELETE` | `/v1/jobs/{job-id}/coupled-simulations/{simulation-id}` | Request cooperative cancellation of an active simulation |

`POST` requires a **completed analysis job**. A saved rainfall scenario is required only for an opt-in `scenario_forcing` request. The accepted response includes a `simulation_id`, a `job_id`, `state`, `result_ready`, `model`, `error`, and work counters. Poll until `state=completed`; `failed`, `canceled` and `interrupted` are terminal. Query snapshots only when `result_ready=true`.

The C++ `AnalysisService` methods mirror the REST methods: `startCoupledSimulation`, `coupledSimulationStatus`, `coupledSimulationSnapshots`, and `cancelCoupledSimulation`. The standalone host uses these methods on its existing service instance; **no localhost request is required**.

A finished simulation is immutable. To vary calibration parameters or the inflow profile, **submit a new simulation**. There is no in-place edit or silent reuse of the old result.

## Minimal request: one reservoir, one free-overflow outlet

This example intentionally uses **illustrative** stage–storage and outlet parameters, not values inferred from Copernicus GLO-30:

```json
{
  "reservoirs": [
    {
      "component_index": 10,
      "initial_storage_m3": 1100,
      "stage_storage": [[0, 0], [10, 1000], [20, 2000]]
    }
  ],
  "connections": [],
  "outlets": [
    {
      "component_index": 10,
      "crest_elevation_m": 10,
      "effective_width_m": 2,
      "discharge_factor": 0.9,
      "downstream_water_level_m": 8
    }
  ],
  "inflows": [
    {"duration_seconds": 10, "volume_m3": [50]},
    {"duration_seconds": 10, "volume_m3": [0]}
  ],
  "options": {"maximum_substep_seconds": 0.1, "maximum_substeps": 1000000}
}
```

Every reservoir has a unique **numeric component index**, a physically defensible stage–storage curve in the **same absolute elevation datum** as its controls, and an initial storage in m³. Stage–storage knots are `[water_elevation_m, stored_volume_m3]`, strictly increasing in both coordinates and starting with zero storage. Reservoirs are *independent hydraulic control volumes*: never include a parent merge-tree basin in addition to the child basins containing the same water, or storage will be counted twice. Inflow `volume_m3` arrays always match **reservoir array order**, not numeric component-index order; their entries must be finite and nonnegative.

`duration_seconds` and all elevations are numeric. Time intervals are processed consecutively, with constant average inflow rate per interval (`volume / seconds`). Snapshot zero is the initial state; the remaining snapshots correspond to interval endpoints. With two inflow intervals, three snapshots are available.

### Internal calibrated connections

The `connections` array may contain `weir` or `orifice` controls. Both require:

- `junction_parent_index`: the **internal depression-hierarchy index** of a junction returned by `extractSpillGeometry()` for this analysis.
- `first_component_index` and `second_component_index`: the two contacting lower-basin components at that junction, represented by the modeled reservoirs.

The service re-extracts the junction contacts from the **saved original DEM and hierarchy**. It refuses a nonexistent junction or unsupported contacts. It does not accept invented junction geometry from HTTP clients.

For a calibrated weir, supply:

```json
{
  "kind": "weir",
  "junction_parent_index": 8,
  "first_component_index": 3,
  "second_component_index": 4,
  "crest_elevation_m": 10,
  "effective_width_m": 2,
  "discharge_factor": 0.9,
  "submergence_rating": [[0, 1], [0.5, 0.8], [1, 0]]
}
```

The weir rating is an externally calibrated tailwater-reduction curve. The `orifice` variant supplies `invert_elevation_m`, `opening_width_m`, `opening_height_m` and `discharge_factor` instead. Its hydraulic model only supports **fully submerged** operation. See [spill hydraulics](SPILL_HYDRAULICS.md), [submerged spill controls](SUBMERGED_SPILL_CONTROLS.md), and [spill geometry](SPILL_GEOMETRY.md). Multiway/diagonal junctions cannot be represented by the existing simple two-component weir. **Raster face-contact length is not an engineered spillway width.**

For a one-way external free-overflow `outlet`, provide a source `component_index`, `crest_elevation_m`, `effective_width_m`, `discharge_factor`, and constant `downstream_water_level_m` **below the crest**. Submerged/reversible boundary conditions are not supported. Do not add the same dry-D8 direct-boundary runoff to both inflow vectors and external hydraulic discharge; these quantities represent different paths. See [boundary outlets](BOUNDARY_OUTLETS.md).

## Optional forcing from a saved rainfall scenario

Replace the `inflows` array with a `scenario_forcing` object containing a saved `scenario_id`, `delivery_model` (`instant` or `d8_constant_velocity`), and a `time_profile` of `{duration_seconds, runoff_fraction}` entries. For travel-time routing also supply `effective_velocity_m_per_s` and `output_interval_seconds`. See [Scenario-driven coupled forcing](SCENARIO_COUPLED_FORCING.md) for a complete request, validation rules, explicit storage calibration, source hashes, and separate direct-runoff versus hydraulic-boundary balances. Existing `inflows` requests and the binary time-series format are unchanged. Both input forms cannot be supplied together.

## Output snapshots

Example query:

```http
GET /v1/jobs/{job-id}/coupled-simulations/{simulation-id}/snapshots?offset=0&limit=25
```

Response contains `model`, `simulation_id`, `total_snapshots`, `offset`, `limit`, `has_more`, and `snapshots`. Each snapshot records:

| Field | Meaning / unit |
|---|---|
| `interval_index` | 0 for initial conditions, then 1, 2, … |
| `elapsed_seconds` | Elapsed modeled time since simulation start (s) |
| `storage_m3` | Per-reservoir **retained** storage, reservoir array order (m³) |
| `water_level_m` | Per-reservoir modeled free-surface elevation (m, source datum) |
| `connection_cumulative_transfer_m3` | Signed cumulative internal transfer, connection array order (m³); positive first→second |
| `boundary_cumulative_outflow_m3` | Per-outlet cumulative discharge, outlet array order (m³) |
| `cumulative_inflow_m3` | Cumulative *supplied* external inflow (m³) |
| `cumulative_boundary_outflow_m3` | Total modeled hydraulic discharge to exterior (m³) |
| `total_storage_m3` | Sum of independently modeled reservoir storage (m³) |
| `mass_balance_error_m3` | Residual of initial storage + inflow − storage − exterior discharge (m³) |

The result also reports accepted/rejected adaptive substeps in the status. A small balance residual demonstrates accounting consistency, **not hydraulic calibration**. Halve the maximum substep size and compare stage/discharge histories to assess first-order time integration sensitivity.

The service bounds requests to 64 reservoirs, 128 connections, 64 outlets, 1,000 inflow intervals, 1,024 stage–storage knots per reservoir, 256 KiB submitted JSON, 8 queued/active tasks, and 100 returned snapshots per request. A dedicated single-worker pool prevents competing simulation tasks from overwhelming the general hydrology job pool. The core's own substep budget and physical-regime validation still apply.

## Persistence, restart and corruption

Each simulation occupies:

```text
<hydrology-output>/<job-id>/coupled-simulations/<simulation-uuid>/
    simulation.json         # immutable request, UUIDs, result state, work counters
    series-v1.aowbin        # atomic, versioned, fixed-width snapshot rows
```

The binary file has a 64-byte header (`AOWHTS01`, format version, array dimensions, snapshot count and SHA-256 of the canonical saved request). Each row carries 32 bytes of SHA-256 and little-endian IEEE-754 `float64` fields. The header, exact expected file size and request digest are validated before reading; **only the requested rows** are checksum-verified for a paginated query. A damaged row causes an error, not fabricated zeroes, and is never silently recomputed through GET. The caller may submit a new independent simulation.

The worker publishes the series with `QSaveFile` before atomically publishing `state=completed` in the manifest. Task state is transient while running; after a restart, completed results remain readable, but a manifest left in `queued`/`running` is reported as `interrupted`. In-progress tasks are cooperatively canceled during clean service shutdown. The simulation is immutable and derived, not the authority for the DEM, model or rainfall scenario.

## Limitations and next integrations

- The API accepts prescribed per-reservoir inflows or an opt-in saved-rainfall forcing adapter. The latter **requires** a user-supplied time profile and (for delayed delivery) effective velocity: it does not infer calibrated hydrographs or hydraulic parameters automatically.
- The result is lumped basin storage and water level. It does **not** provide a dynamic cell-by-cell flood-depth map, street-level hydraulic momentum, wave arrival at arbitrary DEM cells or frictional channel routing.
- It does not implement culvert friction, partial orifice conditions, backwater-driven exterior inflow, moving boundary levels, infiltration, evaporation, or automatically surveyed cross-sections.
- Source basin geometry and user-supplied calibration must share a consistent horizontal and vertical reference. Parent/child merge-tree topology must be interpreted as a hierarchy, not parallel independent reservoirs.
- Next work: expose geometry/calibration selection conveniently to the GUI, add spatially variable rainfall/runoff timing, larger-model profiling and independent reference comparisons.

## Verification

`tests/test_service.cpp` covers submission, invalid requests, status polling, paginated snapshots, mass balance, persistence across restart and detection of a damaged binary row. Core hydraulic behavior and numerical convergence remain tested by `tests/test_coupled_routing.cpp`, `tests/test_boundary_outlets.cpp` and `tests/test_submerged_controls.cpp`.
