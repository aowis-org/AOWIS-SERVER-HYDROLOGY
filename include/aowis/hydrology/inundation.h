#ifndef AOWIS_HYDROLOGY_INUNDATION_H
#define AOWIS_HYDROLOGY_INUNDATION_H

#include <aowis/hydrology/redistribution.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace Aowis::Hydrology
{

// A static STANDING-WATER surface for selected connected, hydrostatic basins.
// This is not a transient flood-depth or sheet-flow model. The conservative
// UnresolvedPartialMerge status has NO depth raster if a parent retains water
// that cannot be assigned to its children or to a connected merged surface.
enum class InundationStatus
{
    Available,
    UnresolvedPartialMerge
};

struct BasinWaterSurface
{
    std::uint64_t basin_index = 0; // the active connected node (root or child)
    double water_level_m = 0.0;
    double stored_volume_m3 = 0.0;
    double inundated_area_m2 = 0.0;
    std::uint64_t inundated_cells = 0;
};

struct StaticInundation
{
    InundationStatus status = InundationStatus::Available;
    // True if one or more sibling basins remain below a merge saddle. The
    // independently reconstructed ponds follow the deterministic volume
    // allocation chosen by redistribution, not a unique hydraulic solution.
    bool allocation_assumed = false;
    // Populated ONLY when status == Available. Cell-centre water depth (m).
    // Dry cells are zero. These depths exclude runoff in transit.
    std::vector<float> depth_m;
    std::vector<BasinWaterSurface> surfaces;
    double reconstructed_volume_m3 = 0.0;
    double inundated_area_m2 = 0.0;
    std::uint64_t inundated_cells = 0;
};

// Inverts the DEM-derived volume/elevation relation for connected root basins.
// At an unformed merge, independently reconstructs the wet children and sets
// allocation_assumed. At a formed merge, computes one connected pond surface.
// If volume cannot be assigned consistently to either case, returns
// UnresolvedPartialMerge with an EMPTY raster. Source DEM and redistribution must
// describe the same analysis. A cell budget bounds memory and computation;
// this is presently an in-process C++ API, not a persisted REST layer.
StaticInundation reconstructStaticInundation(
    const ElevationGrid &original_dem,
    const DepressionHierarchy &hierarchy,
    const RunoffRedistribution &redistribution,
    std::uint64_t maximum_visited_cells = 2000000,
    const std::atomic<bool> *cancel_requested = nullptr);

} // namespace Aowis::Hydrology
#endif
