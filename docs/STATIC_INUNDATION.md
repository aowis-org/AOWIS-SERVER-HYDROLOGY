# Static standing-water reconstruction (Stage 8, first increment)

## Purpose and scope

AOWIS already computes **event-total runoff volume** and allocates that volume to closed depressions in a DEM-derived merge hierarchy. This guide describes the first, deliberately **restricted** conversion from retained basin volume to a physical water level and a raster of **standing-water depth at cell centres**.

The new API is `reconstructStaticInundation()` in `include/aowis/hydrology/inundation.h` and is part of the Qt-independent `AOWIS::Hydrology` library. **The service/REST layer now schedules reconstruction and serves persisted, bounded depth windows; see [INUNDATION_SERVICE.md](INUNDATION_SERVICE.md). There is still no flooded-area polygon or GUI layer.** The implementation rejects insufficiently constrained merged-basin cases instead of filling gaps with guesses. Its data must not be presented as observed or calibrated flood depths.

This reconstructs only water **retained in terrain depressions at static equilibrium**. Water traversing slopes, streams and storm drains is *not* in the raster. Runoff duration and movement times do not influence the result.

## Prerequisites

Inputs must correspond to the **same analysis and original, unmodified terrain**:

1. An `ElevationGrid` with complete finite original elevations and valid WGS84/UTM geometry.
2. The matching `DepressionHierarchy` obtained from `buildDepressionHierarchy()` or the persisted basin archive.
3. The matching `RunoffRedistribution` obtained from `redistributeRunoff()` or the completed scenario cache.

All basin indices and dimensions must agree. The algorithm validates row counts, storage limits and source coverage, but **the caller must still ensure provenance**: a different DEM having identical dimensions is not a valid substitute. Conditioned or breached elevations must not be passed in place of the original source.

## When a water surface is defensible

The reconstruction requires a single hydrostatic surface per active **root basin** (a root has no parent in the depression merge tree). It supports:

- A root basin consisting of a single pit/connected depression leaf, from empty through its boundary spill level.
- A merged root basin after **every immediate child basin is filled to the shared formation saddle**, from that saddle through its root spill level.
- Several mutually independent roots at potentially different water levels.
- A landscape with no retained basin storage: a valid all-zero **standing-water** depth raster. This says nothing about runoff in motion.

A partially filled merge, where one child contains water below the common saddle while another child is at the saddle, is **not uniquely determined** by the existing volume-only solver. The sibling-allocation order is deterministic but not hydraulic. For any wet root in that state, the complete result is:

```text
status == InundationStatus::UnresolvedPartialMerge
depth_m.empty() == true
surfaces.empty() == true
```

This is **not a zero-depth result**. The entire result is unavailable, including other roots, so no user interface can accidentally display only the convenient subset. A future solver may explicitly model local spill trajectories, equilibrium exchange and the time required for connectivity before allowing these cases.

## Method

1. Inspect the **root** volumes in the redistribution tree. Do not sum the stored volumes of all basin nodes: a parent already contains its children's storage.
2. For a merged root, verify that each immediate child is at its recorded spill capacity and that the root contains at least the aggregate child capacity. This is a necessary hydrostatic-connectivity condition.
3. Traverse the original DEM's eight-neighbour **below-spill connected component** from the root minimum. Cell centres at or above the root spill elevation are excluded. This prevents crossing the outlet or inventing a flooded exterior.
4. Compute original-DEM area per row using the existing GeographicLib-backed raster cell metrics. Verify the component's full-spill volume against the hierarchy's recorded capacity; reject inconsistent inputs.
5. Sort the component's elevations and invert its exact piecewise-linear raster-centre hypsometric curve:

   \[V(h) = \sum_{z_i < h} A_i(h-z_i)\]

   where `h` is water-surface elevation (m), `z_i` is source DEM cell-centre elevation (m), and `A_i` is the cell's ellipsoidal area (m²). Between consecutive terrain heights, area stays constant, making the inversion exact for this discrete model—no arbitrary bisection step or artificial epsilon slopes.
6. Write `max(0, h - z_i)` for the eligible connected cells into a row-major `float32` depth raster. Dry cells are zero. A root merge surface must not fall below its formation saddle.
7. Verify that reconstructed root storage agrees with the original redistribution's retained volume, within floating-point tolerance.

**Unit convention:** elevations and depths are metres, areas are square metres, volumes are cubic metres, cell indices are row-major (`index = y * columns + x`). The water surface inherits the vertical datum of the source DEM (Copernicus GLO-30 typically EGM2008), not WGS84 ellipsoidal elevation.

## Worked examples

### One pit

Consider a three-by-three raster with the centre at 10 m and all other cells at 15 m. Let the centre cell area be `A` m². The pit's pre-spill capacity is `5 A` m³. For retained water `2 A` m³, the unique hydrostatic water level is `12 m` and the standing-water depth at the centre is `2 m`. At or above capacity the static water level is capped at `15 m`; the additional event runoff is reported as basin/domain outflow by redistribution, **not** as further water depth inside the analysis window.

### Two neighboring pits

Two 10 m pits separated by a 12 m saddle and bounded by a 16 m outlet initially fill separately. If one leaf is at its 12 m saddle and the other remains partially filled, AOWIS currently **does not publish a depth raster**: the allocation does not justify a common or separate long-term water surface. After both leaves reach their 12 m saddle, a coherent merged basin is possible. A volume above their combined saddle capacity produces one water surface between 12 and 16 m, with depths sampled at all connected cells below that level.

## C++ usage

```cpp
#include <aowis/hydrology/inundation.h>

const Aowis::Hydrology::RunoffRedistribution redistribution =
    Aowis::Hydrology::redistributeRunoff(source_dem, hierarchy, runoff_m3);

const Aowis::Hydrology::StaticInundation result =
    Aowis::Hydrology::reconstructStaticInundation(
        source_dem, hierarchy, redistribution, 2000000);

if (result.status == Aowis::Hydrology::InundationStatus::Available)
{
    // result.depth_m contains one float per source DEM cell.
    // result.surfaces includes each wet connected root's level and area.
}
else
{
    // No depth raster is available. Do not render an all-zero substitute.
}
```

The last argument can be a `const std::atomic<bool>*` cancellation flag. Cell-budget exhaustion or invalid/mismatched inputs throw an exception; the unresolved partial-merge condition is a **normal explicit status** and does not throw. A default budget of two million raster cells bounds one synchronous call's memory/work; the core does not yet support out-of-core or windowed reconstruction.

### Result fields

| Field | Meaning |
| --- | --- |
| `status` | `Available` or `UnresolvedPartialMerge` |
| `depth_m` | Full row-major float32 standing-water depth raster, **only** when available |
| `surfaces` | Root basin array indices, physical water levels, stored volumes and wet-cell areas |
| `reconstructed_volume_m3` | Sum of retained root-basin volumes represented by the reconstruction |
| `inundated_area_m2` | Sum of areas of cells with positive water depth |
| `inundated_cells` | Number of cell centres with positive water depth |

Internal basin indices are not persistent public IDs. When the service layer is implemented, translate them into the existing stable job-scoped basin UUIDs rather than publishing indices as permanent identifiers.

## Accuracy and operational limitations

- The result assumes a **stationary, level water surface within a connected basin**. Hydrostatic conditions may never be reached during the rainfall event.
- There is no flood arrival time, duration, flow velocity, hydrograph, channel capacity, culvert, pump, infiltration kinetics, or subsurface drainage.
- The algorithm reconstructs static ponding in **depressions only**; dry or zero-valued cells could still experience damaging flowing water.
- Pre-spill storage is a raster **cell-centre, eight-neighbour** approximation. It does not interpolate subcell slopes, represent buildings or resolve barriers smaller than the DEM. Float32 raster output introduces additional quantization beyond double-precision volume accounting.
- The original DEM's horizontal and vertical accuracy directly affect the result. **Copernicus GLO-30 is not adequate for engineering-grade street flood-depth prediction.** Small barriers, culverts and inlets can reverse real-world drainage relative to terrain-only routing.
- Artificial DEM boundaries are treated as potential outlets. Expand the selected analysis domain to include relevant upstream and downstream terrain; the result does not predict what happens after water leaves its bounding box.
- Multi-root analysis can consume substantial RAM because the full depth raster, visited flags, connected-cell lists and sorted area/elevation samples are held in memory. Avoid multi-million-cell requests until the planned benchmarks and chunked service delivery are available.
- Independent validation against external terrain hydrology tools and observed events is still pending. Water-volume conservation alone is **not** evidence of flood-depth accuracy.

## Tests and follow-up

`aowis-hydrology-inundation` checks isolated partial/full pit levels, coherent merged roots, explicit rejection of unresolved sibling merges, independent roots, zero-basin behavior, cancellation, cell budgets and missing terrain. The existing `redistribution` tests remain unchanged. Run the repository's full Debug and Release suite:

```bash
./compile_linux_tests.sh --both
```

The **cached and bounded service/REST integration is implemented** (see [INUNDATION_SERVICE.md](INUNDATION_SERVICE.md)). It preserves `UnresolvedPartialMerge` without returning a misleading image. More physically complete inter-basin and transient inundation require a stronger hydraulic model rather than a cosmetic depth interpolation.
