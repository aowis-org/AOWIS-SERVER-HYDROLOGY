# Sibling-spill allocation sensitivity

**Status:** implemented as a pure C++ core diagnostic. The standard redistribution
algorithm, saved scenario results, REST endpoints and cache format are unchanged.
This is **not** a new hydraulic flow solver, an uncertainty probability model,
or a guarantee that all possible allocations have been explored.

## Why this diagnostic exists

When runoff fills a closed depression to its saddle, surplus water can enter
another depression in the same parent basin. If three or more child basins
share a saddle (or have equal-level nested saddles), multiple potential
receivers may exist. The DEM supplies topographic connectivity and capacity,
but it does **not** supply the hydraulic information required to decide how
quickly water will pass along each route.

The current static volume model must nevertheless assign that surplus to
specific children before reconstructing separate pre-merge water surfaces.
Historically it assigned incoming excess to child basins in stored-index
order. This is deterministic but the **choice of receiver** is an assumption,
not a physical prediction. Mass balance can be perfect while the inundated
cells are not uniquely determined.

The diagnostic exposes this situation by re-running the **same** rainfall
runoff raster and original DEM hierarchy with two traversal rules:

- `BasinIndexAscending`: the existing behavior (default, used by cached jobs).
- `BasinIndexDescending`: a deliberate alternative ordering of child basins.

Both runs retain the same leaf-inflow source volumes, dry original-DEM D8
routing, basin capacities, merge elevations, domain boundary and numerical
conservation checks. **Only the order in which a shared-saddle excess fills
eligible children is changed.**

## Interpretation

The diagnostic reports `differing_leaf_basins` with each affected leaf's
`ascending_storage_m3` and `descending_storage_m3`. These are **alternative
volume allocations**, not a confidence interval. `maximum_leaf_difference_m3`
is the largest absolute difference between corresponding leaf volumes.

A leaf is reported when the difference exceeds `1e-9 * max(1 m³, |A|, |B|)`.
This is a numerical reporting threshold; it is not a physical accuracy bound.
Only leaves are compared because adding parent and child volumes would double
count the same stored water. Basin identifiers are the numeric core indices;
translate them using the same job's depression hierarchy when presenting
persistent UUIDs.

`allocation_sensitive=true` **proves that the current sibling allocation
can affect individual pond volumes**, even if overall storage and outflow are
identical. An individual mapped depth derived from either allocation should
not be treated as a unique prediction.

`allocation_sensitive=false` does **not** prove a unique hydraulic solution.
Two traversal orders are merely two possible allocations. Even if they agree,
a different hydraulic resistance, culvert, flow-path width or inflow
hydrograph may change real ponding.

### Conservation contract

For each run:

`generated_runoff_m3 = retained_storage_m3 + boundary_outflow_m3 + mass_balance_error_m3`

The existing solver verifies the residual within its numerical tolerance.
The diagnostic additionally verifies that both allocation rules agree on the
**global** runoff, root-basin storage and total boundary outflow within
`1e-8 * max(1 m³, generated_runoff_m3)`. If that invariant fails, it throws
instead of displaying inconsistent comparisons. It does not assert that the
leaf-level volumes agree.

## Worked example: three pits and one saddle

Imagine three independent pits with floors at 10 m and a common connection
at 12 m. Each pit has equal pre-spill capacity **C**. Runoff amount **1.4 C**
enters the first pit, and no water enters the other two directly.

1. The first pit retains **C** and transfers **0.4 C** over the shared saddle.
2. With ascending child order, the second pit can receive **0.4 C** while the
   third remains empty.
3. With descending child order, the third pit can receive **0.4 C** while the
   second remains empty.
4. Both alternatives retain **1.4 C** and discharge **0** outside the DEM.
   Yet their spatial inundation distributions differ.

The included synthetic test constructs precisely this 10 m / 12 m / 16 m
terrain configuration and verifies both the per-leaf differences and the
identical global balance. It also checks an event big enough to fill the
whole parent: after all pits have merged and reached their capacities, there
is no order-dependent storage remaining.

## C++ usage

```cpp
#include <aowis/hydrology/redistribution.h>

// original_dem and hierarchy refer to the SAME immutable source DEM.
// runoff_m3 is the scenario's nonnegative m³-per-cell raster.
const Aowis::Hydrology::SiblingAllocationSensitivity check =
    Aowis::Hydrology::assessSiblingAllocationSensitivity(
        original_dem, hierarchy, runoff_m3);

if (check.allocation_sensitive)
{
    for (const Aowis::Hydrology::SiblingAllocationDifference &item :
         check.differing_leaf_basins)
    {
        // item.leaf_basin_index identifies a LEAF, not a parent sum.
        // Compare item.ascending_storage_m3 and item.descending_storage_m3.
    }
}
```

For an individual alternative result:

```cpp
const Aowis::Hydrology::RunoffRedistribution alternative =
    Aowis::Hydrology::redistributeRunoffWithSiblingOrder(
        original_dem, hierarchy, runoff_m3,
        Aowis::Hydrology::SiblingAllocationOrder::BasinIndexDescending);
```

Both functions also accept an optional `const std::atomic<bool>*` cancellation
flag. Invalid grid/hierarchy inputs are rejected just as in the default
redistribution API. The explicit order API rejects unrecognized enum values.

## Runtime and system integration

- Sensitivity evaluation runs **two complete redistributions** (including the
  dry D8 graph) and holds both basin-state vectors while comparing them.
  It is intentionally **opt-in**, not part of ordinary cached result reads.
- The authoritative rainfall raster and terrain analysis are not modified.
  The existing `redistributeRunoff()` still means `BasinIndexAscending`.
- No new persisted files, cache invalidation, format migration, REST endpoint
  or Qt dependency is introduced here. The user-facing inundation REST API
  continues to report `allocation_assumed` where appropriate; it does not
  claim that this sensitivity diagnostic has been run.
- Diagnostics for large jobs should eventually be scheduled as separate
  bounded tasks, with a cached or paginated result, rather than executed
  synchronously inside an HTTP GET handler.

## What is still required for hydraulically defensible redistribution

A more physically determined split requires, at minimum, identification of
actual spill channels and barriers at finer resolution, source inflow timing,
flow-path geometry and a physically justified conveyance/resistance model.
Storm-drain connectivity, culverts, walls, and overland 2D flow may dominate
urban water levels; Copernicus GLO-30 cannot resolve many of these features.
A time-dependent shallow-water or linked SWMM calculation would be a separate
model stage. The two-order comparison is **diagnostic evidence of ambiguity**,
not a substitute for those calculations.

## Verification

`tests/test_sibling_allocation.cpp` covers a shared saddle, volume invariance,
changed receiver volumes, the fully saturated parent, an isolated pit,
invalid policy values and cancellation. The existing redistribution suite
continues to exercise unchanged default behavior, mass conservation and
archive compatibility.

```bash
./compile_linux_tests.sh --both
```

See [RUNOFF_REDISTRIBUTION.md](RUNOFF_REDISTRIBUTION.md) for the base volume
algorithm and [STATIC_INUNDATION.md](STATIC_INUNDATION.md) for interpreting
conditional water-level rasters.
