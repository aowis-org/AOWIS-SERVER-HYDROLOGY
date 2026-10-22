# Optional capacity-weighted allocation at shared basin saddles

**Status:** implemented as an opt-in, Qt-independent C++ redistribution policy.
The default, cached redistribution policy is **unchanged**. This is a static
allocation **heuristic**, **not** a hydraulic flow law, runoff timing model,
flow-rate model or validated prediction of where a real flood will pond.

## Problem and scope

A topographic merge tree identifies each closed depression's spill elevation,
its storage capacity below that elevation, and the parent containing the merged
basins. It does **not** identify the velocities, flow widths, culvert capacity,
roughness or hydrographs governing the split of overflow between two eligible
receiving depressions. The existing solver uses an *index-ordered* allocation:
one eligible child may receive all available overflow while another remains
empty. The [sibling-allocation sensitivity diagnostic](SIBLING_ALLOCATION_SENSITIVITY.md)
demonstrates that this affects individual pond volumes without changing
whole-domain water balance.

The new `RemainingCapacityWeighted` policy removes child **traversal order**
as a tie-breaker at a shared saddle. It shares incremental overflow according
to how much storage each eligible child has left **below its own spill level**.
It is useful for exploring the consequences of a symmetric distribution rule;
there is **no implication that larger basins physically attract more water**.

## Algorithm

The source of each cell's runoff, the original-DEM unfilled D8 flow graph,
direct leaf-basin inflows, merge tree, basin capacities, out-of-domain routing
and global conservation checks are the same as the standard solver.

For an internal basin with `n` immediate children:

1. Existing direct rainfall inputs and already-computed child spill volumes
   are accumulated in the normal bottom-up topology order.
2. The basin accepts at most `capacity_at_spill_m3 - already_stored_m3`
   additional cubic metres. Denote this nonnegative amount **A**.
3. For each child `i` compute its remaining storage:
   **Dᵢ = max(0, child_capacityᵢ − child_storedᵢ)**.
4. Let **D = ΣDᵢ** and **S = min(A, D)**. If `D > 0`, allocate the increment
   **Qᵢ = S × Dᵢ / D** to each child. Children with zero deficit receive zero.
   Child identities are traversed in ascending numeric basin-index order for
   reproducibility, independent of how the parent stores `child_indices`.
5. The allocated water descends into each child subtree with the same rule.
   If `A > D`, the excess is held as connected parent storage above the
   children's shared saddle, up to the parent's spill capacity.
6. Anything beyond root capacity exits the DEM boundary. It is **not**
   counted as flow through a specific real-world river or sewer.

The solver calculates these shares from **a snapshot of child deficits before
this increment**. Multiple incoming volumes already accumulated for this
parent form a single increment; no child gets precedence for having a lower
numeric index. The resulting storage state still uses
`stored_in_subtree_m3` for **inclusive** parent-plus-descendant water. Never
sum all nodes to calculate total storage; sum roots only.

**Important:** the weights are **capacity** weights, not saddle widths,
Manning conveyance weights, current surface area weights, or elevation/pressure
head differences. Deep or nested basins can be physically asymmetric, and
capacity weighting cannot recover that missing geometry or hydrodynamics.

### Example with three depressions

Assume three separate leaves join at a 12 m saddle. Leaf A is full at its
pre-spill capacity of **100 m³**, while leaves B and C have remaining
capacities of **60 m³** and **120 m³**. A further **90 m³** of runoff arrives
from A's spill path.

- Existing index-ordered allocation could put **60 m³** into B and the
  remaining **30 m³** into C (or the opposite order).
- Under the capacity-weighted rule, B receives
  **90 × 60 / (60 + 120) = 30 m³**, and C receives
  **90 × 120 / (60 + 120) = 60 m³**.
- A retains 100 m³. The parent retains 190 m³ in total. With enough available
  parent capacity, there is no boundary discharge, regardless of policy.

If B and C have identical deficits, each receives exactly half the spill.
If enough overflow arrives to saturate both, the policies converge on the
same fully merged pond volume. **Neither outcome proves an actual hydraulic
split**; the result only states a reproducible assignment conditional on the
chosen policy.

## C++ API

The explicit policy entry point is:

```cpp
#include <aowis/hydrology/redistribution.h>

const Aowis::Hydrology::RunoffRedistribution weighted =
    Aowis::Hydrology::redistributeRunoffWithSiblingPolicy(
        original_dem, hierarchy, runoff_volume_m3,
        Aowis::Hydrology::SiblingAllocationPolicy::RemainingCapacityWeighted);

// Recover the original persisted-policy behavior without changing the cache:
const Aowis::Hydrology::RunoffRedistribution original =
    Aowis::Hydrology::redistributeRunoffWithSiblingPolicy(
        original_dem, hierarchy, runoff_volume_m3,
        Aowis::Hydrology::SiblingAllocationPolicy::IndexAscending);
```

`original_dem` must contain the unmodified elevation values that produced
`hierarchy`. `runoff_volume_m3` is a nonnegative, finite cubic-metre volume
for **each** raster cell. Both functions accept an optional cancellation
pointer (`const std::atomic<bool>*`), preserve the original input, reject
unknown policy values, and perform the same topology and water-balance
validation as ordinary redistribution. See
[RUNOFF_REDISTRIBUTION.md](RUNOFF_REDISTRIBUTION.md) for prerequisites.

The original APIs remain exactly as before:

- `redistributeRunoff()` and the persistent C++/REST job service continue to
  use the **index-ascending** policy.
- `redistributeRunoffWithSiblingOrder(..., BasinIndexDescending)` remains an
  explicit diagnostic alternative.
- `assessSiblingAllocationSensitivity()` still compares **ascending versus
  descending** only; the weighted policy is an additional third model, not a
  modification of that diagnostic.
- No additional server configuration, HTTP endpoint, task type, cache field,
  binary-format version, database migration or new runtime dependency is
  introduced by this increment.

**Do not persist or display a weighted result as an ordinary scenario result**
without recording which allocation policy generated it. That would make
scenario cache reuse, comparisons and derived depth maps ambiguous. A later
service-level feature must provide policy provenance, versioned cache keys,
result selection, and clear uncertainty flags before exposing weighted depth
layers through the GUI. Existing `allocation_assumed` depth flags still apply
for unformed sibling saddles and are **not** cleared by weighting.

## Conservation, stability and runtime

Every run enforces, within the solver's numerical tolerance:

`generated_runoff_m3 = basin_storage_m3 + total_boundary_outflow_m3 + mass_balance_error_m3`

At each merge, the weighted child quotas sum to the accepted volume up to the
amount that fits in child cavities; the remainder remains in parent storage.
Shares are calculated with double precision and a final rounding correction
when it fits within the last eligible child's deficit. As in the original
solver, a cell's entire event runoff is processed as a static volume, so this
is **not** a time-step-by-time-step equilibration algorithm.

Runtime is approximately the same order as ordinary redistribution: one
complete original-DEM D8 solve, a bottom-up basin traversal, and iteratively
bounded subtree fills. The weighted method additionally sorts each visited
node's direct children by index and calculates a capacity sum. It does not
run the expensive two-policy sensitivity diagnostic unless explicitly asked.
Memory grows with the basin hierarchy and the active iterative traversal,
not with a new full-resolution raster. Cancel long operations with the
optional cancellation flag.

## Modeling boundaries and next work

Capacity weighting removes an **arbitrary software-order preference**, not
uncertainty about the flow route. Results remain conditional in particular
because:

- Surface elevation alone does not specify the hydraulic **time** at which
  a spill crosses a saddle, or the fraction that each receiver collects.
- The original D8 graph is a static terrain routing graph, not a 2D shallow-
  water solver, and runoff `duration_seconds` does not generate a hydrograph.
- Source DEM resolution, roads, embankments, culverts, drains and buildings can
  change both the topology and conveyance of water.
- Leaf storage can differ materially among equally mass-conserving policies.
  A lack of differences in selected policies is **not** a uniqueness proof.

The next physics-oriented work should locate actual saddle transfer paths,
represent spill widths and local slopes and define a defensible conveyance
model, then introduce time-varying inflows/flow rates if required. The SWMM
service and higher-resolution terrain can provide additional information, but
that integration is not part of this incremental core API.

## Tests and verification

`tests/test_sibling_allocation.cpp` checks capacity-weighted shares against
an analytical three-pit case, confirms invariance to child list permutation,
verifies parity with the default API when selecting `IndexAscending`, and
checks volume conservation for 20 randomized landscapes. It also checks
invalid enum values and cancellation. Other redistribution, inundation and
persistence tests must continue passing without changing cached outputs.

```bash
./compile_linux_tests.sh --both
```

See also [STATIC_INUNDATION.md](STATIC_INUNDATION.md) for the meaning of
conditional ponding rasters and [ROADMAP.md](ROADMAP.md) for subsequent stages.
