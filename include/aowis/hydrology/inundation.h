#ifndef AOWIS_HYDROLOGY_INUNDATION_H
#define AOWIS_HYDROLOGY_INUNDATION_H

#include <aowis/hydrology/redistribution.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace Aowis::Hydrology
{

// A static STANDING-WATER surface for selected connected, hydrostatic basins.
// This is not a transient flood-depth or sheet-flow model. The deliberately
// conservative UnresolvedPartialMerge status has NO depth raster: the current
// deterministic sibling allocation cannot justify a unique water surface.
enum class InundationStatus
{
    Available,
    UnresolvedPartialMerge
};

struct BasinWaterSurface
{
    std::uint64_t basin_index = 0; // the active (root) connected basin
    double water_level_m = 0.0;
    double stored_volume_m3 = 0.0;
    double inundated_area_m2 = 0.0;
    std::uint64_t inundated_cells = 0;
};

struct StaticInundation
{
    InundationStatus status = InundationStatus::Available;
    // Populated ONLY when status == Available. Cell-centre water depth (m).
    // Dry cells are zero. These depths exclude runoff in transit.
    std::vector<float> depth_m;
    std::vector<BasinWaterSurface> surfaces;
    double reconstructed_volume_m3 = 0.0;
    double inundated_area_m2 = 0.0;
    std::uint64_t inundated_cells = 0;
};

// Inverts the DEM-derived volume/elevation relation for connected root basins.
// Supports isolated leaf roots and root merge basins only AFTER all immediate
// child cavities reach their common formation saddle. Any wet, partially
// merged root returns UnresolvedPartialMerge and an EMPTY raster (never a
// misleading partial or zero-filled map). Source DEM and redistribution must
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
