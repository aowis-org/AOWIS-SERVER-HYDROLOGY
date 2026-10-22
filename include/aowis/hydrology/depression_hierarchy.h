#ifndef AOWIS_HYDROLOGY_DEPRESSION_HIERARCHY_H
#define AOWIS_HYDROLOGY_DEPRESSION_HIERARCHY_H

#include <aowis/hydrology/analysis.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>

namespace Aowis::Hydrology
{

// One connected ponding basin in a raster-centre, 8-neighbour bathtub model.
// Leaves originate at closed regional minima; a parent basin is formed when
// the water from two or more lower basins first meets at a saddle.
// Internal array indices are deterministic for the same DEM, but are not
// persistent AOWIS identities (those must use UUIDs).
struct DepressionBasin
{
    std::uint64_t index = 0;
    std::int64_t parent_index = -1; // -1 means the basin ultimately drains outside, or is unresolved.
    std::vector<std::uint64_t> child_indices;
    std::uint64_t minimum_cell = 0;
    double minimum_elevation_m = 0.0;
    // The lowest level at which all child basins are one connected pond.
    // For a leaf this equals minimum_elevation_m.
    double formation_elevation_m = 0.0;
    std::uint64_t spill_cell = 0;
    // NaN for unresolved basins isolated by missing source coverage.
    double spill_elevation_m = std::numeric_limits<double>::quiet_NaN();
    bool spills_to_domain_boundary = false;
    // Capacity/area just BELOW spill elevation (saddle-level cells have zero
    // volume). NaN means no finite spill was observed in this DEM domain.
    double capacity_at_spill_m3 = std::numeric_limits<double>::quiet_NaN();
    double area_below_spill_m2 = std::numeric_limits<double>::quiet_NaN();
    std::uint64_t cells_below_spill = 0;
};

struct DepressionHierarchy
{
    std::vector<DepressionBasin> basins;
    // True when at least one valid cell touches the exterior of the supplied
    // raster or a permitted NoData hole. This alone does NOT establish that
    // all catchments upstream of the raster are included.
    bool has_open_boundary = false;
    std::uint64_t valid_cells = 0;
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
};

// Construct a merge tree by flooding the immutable source DEM in ascending
// elevation order. Cells below a water level are connected by 8-neighbours.
// Nested storage is represented by parent/child basin nodes. Volumes are
// ellipsoidal-cell-area * (water elevation - original DEM elevation).
// The supplied grid must meet analyze()'s same validity and geometry rules.
// No artificial Priority-Flood gradients, breaching or routing cells are used.
DepressionHierarchy buildDepressionHierarchy(
    const ElevationGrid &source,
    bool reject_interior_nodata = true,
    const std::atomic<bool> *cancel_requested = nullptr);

struct BasinStorageState
{
    double water_level_m = 0.0;
    double water_volume_m3 = 0.0;
    double inundated_cell_area_m2 = 0.0;
    std::uint64_t inundated_cells = 0;
};

// Compute a connected-component water level / volume sample for a basin.
// Levels must lie in [formation, spill]; at spill, returns the exact pre-spill
// capacity stored in the hierarchy. Below spill this visits cells from the
// basin minimum across cells with height <= level and never fills disjoint
// depressions. The sample is a static topographic approximation, NOT a timed
// rainfall or shallow-water solver. max_visited_cells bounds query memory/time.
BasinStorageState basinStorageAtLevel(
    const ElevationGrid &source, const DepressionHierarchy &hierarchy,
    std::uint64_t basin_index, double water_level_m,
    std::uint64_t max_visited_cells = 2000000,
    const std::atomic<bool> *cancel_requested = nullptr);

} // namespace Aowis::Hydrology
#endif
