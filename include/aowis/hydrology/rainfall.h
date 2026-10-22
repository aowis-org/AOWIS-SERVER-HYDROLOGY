#ifndef AOWIS_HYDROLOGY_RAINFALL_H
#define AOWIS_HYDROLOGY_RAINFALL_H

#include <aowis/hydrology/spatial_queries.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace Aowis::Hydrology
{

// One event over the union of its WGS84 polygons (holes exclude land).
// Depth is a total event depth, not an intensity; 0 <= fraction <= 1.
// No loss model or hydrograph is inferred from duration.
struct RainfallScenario
{
    std::vector<GeographicPolygon> polygons;
    double depth_mm = 0.0;
    double duration_seconds = 0.0;
    double effective_runoff_fraction = 1.0;
};

struct RainfallRunoff
{
    // Dense row-major arrays on exactly the elevation grid's footprint.
    // NoData cells are zero, never represented as fabricated catchment area.
    std::vector<float> covered_fraction;
    std::vector<double> runoff_volume_m3;
    std::uint64_t wet_cells = 0;
    double covered_area_m2 = 0.0;
    double gross_rainfall_volume_m3 = 0.0;
    double effective_runoff_volume_m3 = 0.0;
    double retained_or_lost_volume_m3 = 0.0;
    double average_effective_runoff_m3_s = 0.0;
    bool touches_domain_boundary = false;
    bool contains_missing_dem_coverage = false;
};

// Project polygon vertices to the DEM's CRS and compute a geometric union
// of clipped polygon/cell intersections. This avoids doubling up overlaps,
// and respects holes and partial cells. For geographic and UTM grids the
// coverage fraction is measured in grid coordinates and multiplied by the
// row's WGS84-ellipsoid-derived cell area (small-cell approximation).
//
// Bounds prevent untrusted/oversized geometry from consuming uncontrolled
// memory or CPU. The input DEM must have the original, unmodified heights.
RainfallRunoff calculateRainfallRunoff(
    const ElevationGrid &grid, const RainfallScenario &scenario,
    std::uint64_t maximum_examined_cells = 500000,
    const std::atomic<bool> *cancel_requested = nullptr);

} // namespace Aowis::Hydrology
#endif
