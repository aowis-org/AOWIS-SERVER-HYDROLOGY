#ifndef AOWIS_HYDROLOGY_REDISTRIBUTION_H
#define AOWIS_HYDROLOGY_REDISTRIBUTION_H

#include <aowis/hydrology/depression_hierarchy.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace Aowis::Hydrology
{

// A volume-only, instantaneous Fill–Spill–Merge-style approximation.
// The original DEM is used for BOTH dry D8 routing and physical storage;
// this calculation deliberately ignores optional DEM fill/breach settings.
// It neither predicts flood arrival times nor produces an inundation raster.
struct BasinRedistributionState
{
    std::uint64_t basin_index = 0;
    // Direct runoff routed to this leaf. Zero for internal merge nodes.
    double direct_inflow_m3 = 0.0;
    // Total water held in this basin's connected subtree, INCLUDING its
    // children. Never sum these across all nodes (would double count).
    double stored_in_subtree_m3 = 0.0;
    // Volume passed from the basin to its parent, or out of the DEM at a
    // boundary spill. Transfer between siblings is NOT counted as outflow.
    double overflow_m3 = 0.0;
    bool at_spill = false;
};

struct RunoffRedistribution
{
    std::vector<BasinRedistributionState> basins;
    double generated_runoff_m3 = 0.0;
    double basin_storage_m3 = 0.0; // roots only; no double-counting
    double direct_boundary_outflow_m3 = 0.0;
    double basin_boundary_outflow_m3 = 0.0;
    double total_boundary_outflow_m3 = 0.0;
    double mass_balance_error_m3 = 0.0;
    // True whenever the calculation meets the edge of its DEM. This is NOT
    // a verified river or catchment outlet in the real world.
    bool domain_boundary_is_artificial = true;
};

// Incoming overflow can be shared by several sibling depressions at a
// common saddle. With no flow-rate/roughness model the distribution is not
// unique. These two orders are deliberately alternative *allocation rules*,
// NOT lower/upper hydraulic bounds or different time-stepping solvers.
enum class SiblingAllocationOrder
{
    BasinIndexAscending,
    BasinIndexDescending
};

struct SiblingAllocationDifference
{
    std::uint64_t leaf_basin_index = 0;
    double ascending_storage_m3 = 0.0;
    double descending_storage_m3 = 0.0;
};

struct SiblingAllocationSensitivity
{
    // Only leaf storage is compared; summing nested subtree differences
    // would count the same water repeatedly.
    std::vector<SiblingAllocationDifference> differing_leaf_basins;
    double maximum_leaf_difference_m3 = 0.0;
    double generated_runoff_m3 = 0.0;
    double retained_storage_m3 = 0.0;
    double boundary_outflow_m3 = 0.0;
    // If false, agreement under these two rules does NOT prove uniqueness:
    // there may be more than two admissible sibling allocations.
    bool allocation_sensitive = false;
};

// Return a deterministic redistribution using a selected sibling traversal.
// All other model assumptions are identical to redistributeRunoff().
RunoffRedistribution redistributeRunoffWithSiblingOrder(
    const ElevationGrid &original_dem,
    const DepressionHierarchy &hierarchy,
    const std::vector<double> &runoff_volume_m3,
    SiblingAllocationOrder order,
    const std::atomic<bool> *cancel_requested = nullptr);

// Diagnostic: recompute the same scenario in BOTH sibling orders. It does
// not change persistent scenario results, cache format, or the default API.
// Two complete solves are required; use explicitly, not on every REST read.
// Reports alternate LEAF allocations only, and is not a confidence interval.
SiblingAllocationSensitivity assessSiblingAllocationSensitivity(
    const ElevationGrid &original_dem,
    const DepressionHierarchy &hierarchy,
    const std::vector<double> &runoff_volume_m3,
    const std::atomic<bool> *cancel_requested = nullptr);

// All valid cells must have finite original elevations; every invalid DEM
// cell is rejected rather than silently becoming an outlet. Runoff units are
// m3/cell, with one nonnegative finite value for every input raster cell.
// The hierarchy must be built from precisely the same original elevations.
// Values are assumed to reach their downstream sinks instantaneously; this
// is a static volume balance without infiltration or storm timing.
// Throws on incomplete basins, invalid topology, and inconsistent input.
RunoffRedistribution redistributeRunoff(
    const ElevationGrid &original_dem,
    const DepressionHierarchy &hierarchy,
    const std::vector<double> &runoff_volume_m3,
    const std::atomic<bool> *cancel_requested = nullptr);

} // namespace Aowis::Hydrology
#endif
