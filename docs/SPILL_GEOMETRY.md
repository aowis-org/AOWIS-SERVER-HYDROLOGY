# Spill-saddle geometry from the original terrain

## Purpose and status

`AOWIS::Hydrology` can now extract **raster-topological contacts at internal depression-merge saddles**. This is a geometry diagnostic and an input to the separate [opt-in hydraulic control primitive](SPILL_HYDRAULICS.md); **this geometry function itself is not a flow-rate model**.

The existing basin hierarchy tells us **at which elevation** two or more basins become connected. It does not fully describe **where and how** their flooded regions meet. That distinction matters: a corner-only D8 connection is not a surveyed channel, while a long contact boundary is not necessarily a broad, unobstructed spillway.

This component reconstructs that missing geometric information without changing depression storage, runoff redistribution, persisted job files, or inundation behavior. The current default index-ascending redistribution and optional capacity-weighted heuristic are unaffected.

## Data and interfaces

Use `include/aowis/hydrology/spill_geometry.h` and link `AOWIS::Hydrology`:

```cpp
#include <aowis/hydrology/spill_geometry.h>

using namespace Aowis::Hydrology;

// `original_dem` is a complete raster in its ORIGINAL, unfilled elevations.
const DepressionHierarchy hierarchy = buildDepressionHierarchy(original_dem);
const SpillGeometry geometry = extractSpillGeometry(original_dem, hierarchy);

for (const BasinSpillJunction &junction : geometry.junctions)
{
    // junction.parent_basin_index identifies the topmost merge at this level.
    for (const BasinSpillContact &contact : junction.child_contacts)
    {
        // contact.child_basin_index identifies a connected component that
        // already existed strictly BELOW the saddle elevation. It can be a
        // descendant of the parent, not necessarily an immediate child.
        // contact.cardinal_boundary_length_m is a RASTER boundary metric,
        // NOT a calibrated spillway width or discharge coefficient.
    }
}
```

The function is Qt-independent, accepts both geographic WGS84 and WGS84 UTM rasters, and returns C++ values rather than writing a cache. The caller owns the terrain and hierarchy; no Map or HTTP calls occur inside this method.

| Field | Meaning |
| --- | --- |
| `parent_basin_index` | Internal hierarchy node representing the merged component (not a persistent UUID) |
| `saddle_elevation_m` | Level at which the connected components first meet, in the DEM's **original vertical datum** |
| `saddle_cell` | Representative raster cell at that merge level |
| `plateau_cells` | Number of cells at this elevation belonging to the junction's now-connected flooded component |
| `child_basin_index` | Hierarchy index of a component that was separate **below** the saddle; equal-height nested merges may make this a descendant |
| `representative_saddle_cell` / `representative_child_cell` | One touching cell pair, useful for debugging and further sampling |
| `representative_contact_wgs84` | Midpoint of that pair's cell centres, longitude/latitude degrees |
| `cardinal_contact_faces` | Number of side-sharing saddle/lower-cell neighbour pairs |
| `diagonal_contact_pairs` | Number of corner-sharing saddle/lower-cell neighbour pairs |
| `cardinal_boundary_length_m` | Sum of raster-face lengths, using GeographicLib WGS84 row metrics |

There is no invented effective width for diagonal-only contacts: their cardinal length is **exactly zero**. A reported WGS84 coordinate is a representative contact location, not a surveyed culvert, channel centreline or guaranteed pour point.

### Formation saddle versus basin outlet

A subtle but essential detail of `DepressionBasin`:

- **Parent `formation_elevation_m`** records when its children first become connected.
- **Parent `spill_cell` and `spill_elevation_m`** describe its *later outlet* to the next higher parent or the DEM boundary.
- The **direct children's `spill_cell`** identifies the plateau where their parent formed.

Using a parent's own `spill_cell` to locate its *formation* would produce incorrect geometry. This extractor resolves the formation level using the child spill records and verifies their height against the original raster.

### Equal-elevation merges

The merge-tree implementation can contain a chain of several parent nodes all formed at the **same height**. Such nodes reflect a deterministic sequence of component unions, not necessarily several independent physical spillways.

The geometry extractor collapses these equal-height parent/child chains to the **topmost junction at that height**. It identifies the lower connected components existing just below the level and records their contacts with all newly flooded saddle cells. Two saddle-plateau pieces connected through a pre-existing lower component belong to the **same** flooded junction, even when the plateau pieces do not touch each other directly.

Thus the result can report more connected lower components than the chosen topmost parent has immediate children. This is intentional; downstream clients must use `child_basin_index` as a basin-hierarchy referent, not assume a direct parent relationship. Child-list ordering has no effect on the reported contacts.

## Algorithm and costs

1. Validate raster dimensions/geometry and the hierarchy's basic coverage.
2. Sort finite source cells by elevation, with cell index as deterministic tie-breaker.
3. Sweep elevations upward using an eight-neighbour disjoint-set (union-find) structure.
4. Before activating each elevation group containing a merge saddle, record all contacts to connected components already below the level.
5. Activate and union the equal-height cells, including paths through already flooded lower cells. This identifies the actual newly connected flooded components.
6. Associate the lower components with their basin-hierarchy descendants, collapse equal-height merge nodes, and count the cardinal and diagonal contacts separately.
7. Convert representative contacts to WGS84 and approximate cardinal face lengths using the established GeographicLib-based raster metrics.

Sorting costs **O(N log N)** for N valid cells. The sweep is approximately linear thereafter, with union-find and per-row geodesic work. Memory is **O(N)** for the elevation order, union-find arrays and bounded per-elevation contact lists. This is currently an **in-memory** operation, so metropolitan DEM benchmarks are still required before adding persistent or REST-facing geometries.

Two cumulative limits are available:

```cpp
const SpillGeometry geometry = extractSpillGeometry(
    original_dem, hierarchy,
    2000000,   // maximum reported saddle plateau cells across junctions
    16000000); // maximum recorded saddle/lower contact pairs
```

A cancellation flag may be passed as the fifth argument. Limits and cancellation raise exceptions. The contact-pair budget also bounds intermediate per-elevation collection; exceptionally large elevation plateaus may reach it even if only part of the plateau belongs to a requested junction.

## Worked grid example

Consider the following 5×5 elevation grid in metres. The three `10` cells are closed minima, the centre `12` is their common saddle, and the surrounding `16` cells are higher:

```text
16 16 16 16 16
16 10 16 10 16
16 16 12 16 16
16 16 10 16 16
16 16 16 16 16
```

At 12 m, the three minima first become D8-connected. The geometry diagnostic reports one saddle cell with three lower-component contacts. The two upper contacts are **diagonal-only**; the lower contact shares one cardinal raster face.

Therefore, the contacts **do not** describe three equivalent physical channels. No flow-rate split should be derived merely from the fact that each child has a contact. For another grid with `5 – 10 – 5` across one row, each lower basin touches the `10` saddle along one cardinal raster face. These cases have different raster contact geometry even though both merge trees contain two or more children.

## What cannot be inferred

- **No discharge or flow speed.** A weir/channel discharge relationship also needs surveyed or defensible effective opening width, water-head difference, geometry of the controlling section, discharge/roughness coefficients, and time-dependent storage changes. Cell-edge sums do not supply these quantities.
- **No hydraulic width from 8-neighbour corners.** A corner contact has no shared cell edge and may not transmit water in a real urban landscape without a physical channel.
- **No timing.** Rainfall duration is stored by scenarios, but the current static redistribution does not calculate storm hydrographs, delay, attenuation or backwater effects.
- **No street-scale interpretation from GLO-30.** Buildings, road crowns, underpasses, storm drains, culverts and embankment openings may be absent or misrepresented. Use higher-resolution surveyed DEM/structure information where appropriate.
- **No validated domain outfall.** This diagnostic covers internal merge saddles, **not** root-basin exits at analysis-domain boundaries. An analysis edge is not a verified downstream physical outlet.
- **No exact survey width.** `cardinal_boundary_length_m` is a raster-grid contact measure, not an independent measurement of a hydraulic cross-section. The existing row-based UTM metrics also use an approximate local projection scale.
- **No complete source-content fingerprint.** It checks dimensions, finite coverage and local terrain/hierarchy relationships, but cannot prove whole-DEM identity from a hierarchy that currently stores no DEM fingerprint. Use the authoritative persisted analysis/DEM association when invoking it.

## Verification and next steps

The dedicated `aowis-hydrology-spill-geometry` test covers cardinal versus diagonal interfaces, connected multi-cell plateaus, equal-height but disconnected saddles, randomized nested equal-height merges, WGS84/UTM coordinates, budgets, cancellation and invalid terrain. Existing redistribution/volume-balance tests must continue to pass unchanged.

The original [free-overflow broad-crested control module](SPILL_HYDRAULICS.md) requires **independently supplied** effective width/discharge calibration and a two-way cardinal contact; its original methods still refuse submerged cases. Separate opt-in [calibrated-tailwater and fully submerged engineered-opening controls](SUBMERGED_SPILL_CONTROLS.md) now offer additional rate models, **without** using DEM contact lengths as surveyed widths or inventing culvert dimensions. No control here is connected to the default static redistribution cache or REST service. Full coupled stage–storage routing still requires measured/calibrated control geometry, dynamic inflows and a convergent time integrator. The capacity-weighted or index-order policy remains a transparent static approximation, not a physical prediction.
