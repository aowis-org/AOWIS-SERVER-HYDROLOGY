# Static standing-water depth reconstruction

**Allocation uncertainty:** Separate pre-merge child surfaces use the
volumes assigned by the redistribution solver. When a shared saddle has
multiple receivers, that allocation may depend on child visitation order.
The opt-in C++ [sibling allocation sensitivity diagnostic](SIBLING_ALLOCATION_SENSITIVITY.md)
compares two alternative orders and lists affected leaf ponds. This does not
make either raster a hydraulic prediction, and `allocation_assumed` does not
mean that the diagnostic has been run.

## Purpose

The Hydrology service converts **retained runoff volume**, produced by the static Fill–Spill–Merge-style redistribution, into estimated pond water levels and cell-centre depth values. It uses the **original DEM**, not the depression-filled or breached D8 processing surface. Its physical domain is **water stored in closed topographic depressions**, not water moving across slopes or through infrastructure.

The reusable C++ entry point is `reconstructStaticInundation()` in `include/aowis/hydrology/inundation.h`; the asynchronous, persisted HTTP/C++ service is described in [INUNDATION_SERVICE.md](INUNDATION_SERVICE.md). Neither interface solves time-varying hydraulics. Results are screening-level, especially with 30 m satellite elevation data, and must not be interpreted as validated flood forecasts.

## Inputs and provenance

The caller provides:

1. A complete finite original `ElevationGrid`, in a supported raster geometry/vertical datum.
2. The matching `DepressionHierarchy` (basin topology, spill/formation elevations and capacities).
3. A matching `RunoffRedistribution` (retained volume for each basin subtree and the domain volume balance).

All three must originate from the **same analysis job and exact original DEM**. Geometry/dimension and selected elevation checks reject many mismatches, but do not replace source-revision tracking. The persistent service ties results to saved source fingerprints and ignores stale cache versions.

## The connectivity model

A depression hierarchy is a **merge tree**, not necessarily one flooded pond. Every node has a spill elevation; internal nodes have a formation saddle at which their child cavities first connect.

The reconstruction traverses each root **iteratively**, without recursion:

- **Leaf below its spill saddle:** reconstruct that pit alone from its retained volume.
- **Internal node whose children have all reached their common saddle:** reconstruct its connected cavity as **one** level surface. Higher-level parent capacity includes the volumes in all descendants.
- **Internal node not yet connected:** if all of its volume is accounted for in its child subtrees, descend to those children and reconstruct **separate** levels for the wet branches. Set `allocation_assumed = true`.
- **Inconsistent allocation:** if an unconnected parent contains volume that cannot be accounted for in its children, the result is `UnresolvedPartialMerge` **with no depth raster**. It is never silently treated as dry land.

At every level, only the highest appropriate **active** connected nodes are reconstructed. A selected parent and its children are *never* both counted. Multiple independent analysis-domain roots are handled the same way.

### Interpretation of `allocation_assumed`

The existing runoff solver assigns excess water among same-saddle siblings **deterministically**, using stored tree order. This is useful and volume-conserving but **does not independently establish the real hydraulic split** between cavities: arrival times, constriction capacities, spill velocities, and a complete hydraulic equilibration model are absent.

The core also provides an opt-in [capacity-weighted sibling allocation
heuristic](CAPACITY_WEIGHTED_SIBLING_ALLOCATION.md). Its use would likewise
require `allocation_assumed = true` for unformed saddles; a symmetric split
is not necessarily the real hydraulic split. The current REST depth cache
continues to derive from the **original index-ascending** redistribution.

When `allocation_assumed = true`, the depth raster answers:

> **Given the runoff volume that our redistribution model assigned to each not-yet-connected child basin, what standing-water depths follow from those allocations?**

It does **not** establish that real floodwater would divide in exactly that proportion. GUI clients should flag such layers as *conditional pre-merge ponding*, not display them as uniquely predicted depths. `allocation_assumed = false` means the reconstruction did not rely on an unformed sibling merge; it does **not** imply that the underlying rainfall, terrain or routing model is calibrated.

## Water level and depth calculations

For each selected active basin, traverse original DEM cells connected to its minimum by 8-neighbour links **strictly below that basin's spill elevation**. In particular, the outer spill saddle is not included as a water-bearing cell, preventing an artificial flooded connection to the surrounding terrain.

Each cell has DEM-centre height `z_i` (metres) and source-grid area `A_i` (m²), calculated using the GeographicLib-backed geometry metrics. Within this discrete model, volume as a function of level `h` is

\[
V(h) = \sum_{z_i < h} A_i (h-z_i).
\]

Cells are sorted by height and `V(h)` is inverted exactly by sweeping consecutive height groups. This avoids iterative height guessing and subtracting two very large absolute elevation products to calculate a small depression volume.

At each contributing cell, the saved depth is `max(0, h-z_i)` in **float32 metres**; outside the selected active components, it is zero. The physical volume and the quantized raster volume are both checked against the assigned retained storage with separate tolerances. A mismatch is an error, not a partially published image. Surface elevation inherits the DEM vertical datum (Copernicus GLO-30 commonly EGM2008); depth is a vertical difference in metres.

## Worked examples

### One pit

A 3×3 DEM has a 10 m centre pixel surrounded by 15 m edge pixels. If its physical cell area is `A` m², maximum pre-spill storage is `5A` m³. Assigned storage `2A` m³ produces a **12 m** surface and **2 m** centre-cell depth. At `5A` m³ the pond reaches its 15 m boundary spill. Further runoff is **outflow**, not ever-increasing centre-cell depth.

### Two sibling pits at a 12 m saddle

Two 10 m pits are separated by a 12 m saddle and bounded by a 16 m outlet. Each leaf's pre-merge capacity is approximately `2A` m³ for its minimum cell (assuming equal cell areas).

- Left assigned `2A`, right assigned `A`: levels are **12 m** and **11 m** respectively. The result contains **two separate surfaces**, `allocation_assumed = true`. Those *positions* follow the model's chosen sibling volumes, not a measured flow split.
- Both leaves assigned `2A` and their parent receives additional retained water: the common saddle is reached; the parent is now one connected pond with a shared level **at or above 12 m**. The result uses the merged node, not duplicate child water volumes.

### Nested merges

If two depressions connect at 12 m and that pair can only connect to a third pit at 14 m, the first pair may already share a common level while the third remains lower. The algorithm selects the 12 m merged child plus the separately wet third leaf. It does not incorrectly impose one 14 m-or-higher level across all three.

## Results and C++ usage

```cpp
#include <aowis/hydrology/inundation.h>

const Aowis::Hydrology::RunoffRedistribution volumes =
    Aowis::Hydrology::redistributeRunoff(original_dem, hierarchy, runoff_m3);
const Aowis::Hydrology::StaticInundation ponding =
    Aowis::Hydrology::reconstructStaticInundation(
        original_dem, hierarchy, volumes, 2000000);

if (ponding.status == Aowis::Hydrology::InundationStatus::Available)
{
    // ponding.depth_m: float32, one value per source DEM cell.
    // ponding.surfaces: active basin nodes, not necessarily only roots.
    // ponding.allocation_assumed: flag conditional pre-merge assignments.
}
else
{
    // No usable depth raster. Do not display a substitute zero layer.
}
```

| Field | Semantics |
| --- | --- |
| `status` | `Available` or `UnresolvedPartialMerge` |
| `allocation_assumed` | `true` when an unformed merge required deterministic sibling allocations |
| `surfaces` | Active connected nodes: basin index, level (m), storage (m³), wet area (m²) and cell count |
| `depth_m` | Row-major source-grid float32 standing-water depths, **only** when available |
| `reconstructed_volume_m3` | Counted once across active nodes; must match redistribution's retained root storage |
| `inundated_area_m2`, `inundated_cells` | Cells with **positive** reconstructed depth |

Basin indices are internal graph positions. The service converts them to stable, job-scoped basin UUIDs in its JSON surface list. `maximum_visited_cells` defaults to two million, bounding the total visited-cell work; callers may supply an optional `const std::atomic<bool>*` cancellation flag. Failures in input consistency, capacity, numerical checks, cell budget, or cancellation throw an exception.

## Fundamental limitations

- The model is **static event-total pond storage**, not a storm hydrograph. Even a valid local hydrostatic level might never be reached during the event.
- **Pre-merge allocations are a model assumption.** Conservation of water does not validate how much of it enters each sibling. The `allocation_assumed` flag makes that limitation explicit.
- Zero depth means **no retained depression storage at that cell centre**; it does not mean zero surface flow, zero hazard, or successful storm-drain capture.
- An 8-neighbour cell-centre raster is not a true subcell hydraulic surface; small berms, culverts, inlets, buildings and road levels can dominate local flooding and may be missing from Copernicus GLO-30.
- DEM domain edges are artificial drainage boundaries. Missing upstream terrain, a cut-through watershed or a truncated outlet changes the water balance.
- Double-precision volume accounting and float32 depth serialization have different numeric tolerances. GeographicLib cell areas remain an approximation to actual subcell topography.
- Full-grid reconstruction has nontrivial RAM and sorting costs. The service reads depth windows efficiently **after** the full raster has been computed; reconstruction itself is not out of core.
- Comparison against independent terrain solvers and observed flood events remains pending. The model must not be used for infrastructure safety decisions without independent validation.

## Verification and follow-up

`aowis-hydrology-inundation` covers isolated pits, full spills, partially filled siblings, nested two-stage merges, mass balance, deliberately inconsistent parent allocations, multiple independent roots, no-basin terrain, cancellation and budget rejection. Run the full Debug and Release suite using `./compile_linux_tests.sh --both`.

The HTTP/cached integration is described in [INUNDATION_SERVICE.md](INUNDATION_SERVICE.md); the assignment of water between siblings is documented in [RUNOFF_REDISTRIBUTION.md](RUNOFF_REDISTRIBUTION.md). A future physically stronger solver must account for saddle transfer trajectories and hydraulic timing before these conditional allocations could be promoted to calibrated flood predictions.
