#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/inundation.h>
#include <aowis/hydrology/geometry.h>

#include <cassert>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace Aowis::Hydrology;

namespace
{
ElevationGrid grid(std::uint32_t columns, std::uint32_t rows,
                   const std::vector<float> &heights)
{
    ElevationGrid result;
    result.columns = columns;
    result.rows = rows;
    result.geometry.origin_x = 8.0;
    result.geometry.origin_y = 50.0;
    result.geometry.pixel_width = 0.001;
    result.geometry.pixel_height = 0.001;
    result.geometry.vertical_datum = VerticalDatum::Egm2008;
    result.elevations_m = heights;
    return result;
}
bool near(double left, double right)
{
    return std::abs(left - right) <= 1e-6 * std::max({1.0, std::abs(left), std::abs(right)});
}
}

int main()
{
    const ElevationGrid single = grid(3, 3, {
        15, 15, 15,
        15, 10, 15,
        15, 15, 15
    });
    const DepressionHierarchy single_basins = buildDepressionHierarchy(single);
    assert(single_basins.basins.size() == 1);
    const double capacity = single_basins.basins[0].capacity_at_spill_m3;
    std::vector<double> storm(9, 0.0);
    storm[4] = capacity * 0.4;
    const RunoffRedistribution partial = redistributeRunoff(single, single_basins, storm);
    const StaticInundation partial_depth = reconstructStaticInundation(single, single_basins, partial);
    assert(partial_depth.status == InundationStatus::Available);
    assert(partial_depth.surfaces.size() == 1);
    assert(near(partial_depth.surfaces[0].water_level_m, 12.0));
    assert(near(partial_depth.depth_m[4], 2.0));
    assert(partial_depth.inundated_cells == 1);
    assert(near(partial_depth.reconstructed_volume_m3, capacity * 0.4));
    for (std::size_t i = 0; i < partial_depth.depth_m.size(); ++i)
        if (i != 4) assert(partial_depth.depth_m[i] == 0.0f);

    storm[4] = capacity * 2.0;
    const RunoffRedistribution overflowing = redistributeRunoff(single, single_basins, storm);
    const StaticInundation full = reconstructStaticInundation(single, single_basins, overflowing);
    assert(full.status == InundationStatus::Available);
    assert(near(full.surfaces[0].water_level_m, 15.0));
    assert(near(full.depth_m[4], 5.0));
    assert(near(full.reconstructed_volume_m3, capacity));

    // Two cavities on opposite sides of a common 12 m saddle, 16 m outlet.
    const ElevationGrid paired = grid(7, 5, {
        16, 16, 16, 16, 16, 16, 16,
        16, 16, 16, 16, 16, 16, 16,
        16, 10, 12, 12, 12, 10, 16,
        16, 16, 16, 16, 16, 16, 16,
        16, 16, 16, 16, 16, 16, 16
    });
    const DepressionHierarchy merged = buildDepressionHierarchy(paired);
    assert(merged.basins.size() == 3);
    const double leaf_capacity = merged.basins[0].capacity_at_spill_m3;
    const double parent_capacity = merged.basins[2].capacity_at_spill_m3;
    std::vector<double> merged_storm(35, 0.0);
    merged_storm[15] = leaf_capacity * 1.5;
    const RunoffRedistribution incomplete = redistributeRunoff(paired, merged, merged_storm);
    const StaticInundation unresolved = reconstructStaticInundation(paired, merged, incomplete);
    assert(unresolved.status == InundationStatus::UnresolvedPartialMerge);
    assert(unresolved.depth_m.empty());
    assert(unresolved.surfaces.empty());

    merged_storm[15] = parent_capacity * 0.75;
    const RunoffRedistribution connected = redistributeRunoff(paired, merged, merged_storm);
    const StaticInundation merged_depth = reconstructStaticInundation(paired, merged, connected);
    assert(merged_depth.status == InundationStatus::Available);
    assert(merged_depth.surfaces.size() == 1);
    assert(merged_depth.surfaces[0].basin_index == 2);
    assert(merged_depth.surfaces[0].water_level_m >= 12.0);
    assert(merged_depth.surfaces[0].water_level_m < 16.0);
    assert(merged_depth.depth_m[15] > 0.0f);
    assert(merged_depth.depth_m[19] > 0.0f);
    assert(near(merged_depth.reconstructed_volume_m3, connected.basin_storage_m3));
    double grid_volume = 0.0;
    for (std::uint64_t cell = 0; cell < merged_depth.depth_m.size(); ++cell)
        grid_volume += double(merged_depth.depth_m[std::size_t(cell)]) *
            rasterRowCellAreaM2(paired.geometry, paired.columns,
                                std::uint32_t(cell / paired.columns));
    assert(near(grid_volume, connected.basin_storage_m3));

    merged_storm[15] = parent_capacity * 1.5;
    const RunoffRedistribution full_merge = redistributeRunoff(paired, merged, merged_storm);
    const StaticInundation full_merge_depth = reconstructStaticInundation(paired, merged, full_merge);
    assert(full_merge_depth.status == InundationStatus::Available);
    assert(near(full_merge_depth.surfaces[0].water_level_m, 16.0));
    assert(near(full_merge_depth.reconstructed_volume_m3, parent_capacity));

    // Two independent closed basins with separate boundary saddles should
    // produce distinct simultaneous water levels, without any merging.
    const ElevationGrid independent = grid(9, 5, {
        20, 14, 20, 20, 25, 20, 20, 14, 20,
        20, 13, 20, 20, 25, 20, 20, 13, 20,
        20, 10, 20, 20, 25, 20, 20, 11, 20,
        20, 13, 20, 20, 25, 20, 20, 13, 20,
        20, 20, 20, 20, 25, 20, 20, 20, 20
    });
    const DepressionHierarchy independent_hierarchy = buildDepressionHierarchy(independent);
    std::vector<double> independent_storm(45, 0.0);
    independent_storm[19] = 1500.0;
    independent_storm[25] = 3500.0;
    const RunoffRedistribution separate = redistributeRunoff(
        independent, independent_hierarchy, independent_storm);
    const StaticInundation independent_depth = reconstructStaticInundation(
        independent, independent_hierarchy, separate);
    assert(independent_depth.status == InundationStatus::Available);
    assert(independent_depth.surfaces.size() == 2);
    assert(independent_depth.depth_m[19] > 0.0f);
    assert(independent_depth.depth_m[25] > 0.0f);
    assert(near(independent_depth.reconstructed_volume_m3, separate.basin_storage_m3));

    // No basin means no retained standing water, not a claim about flow in transit.
    const ElevationGrid slope = grid(3, 3, {
        10, 10, 10,
        10, 12, 10,
        10, 10, 10
    });
    const DepressionHierarchy none = buildDepressionHierarchy(slope);
    std::vector<double> slope_storm(9, 1.0);
    const RunoffRedistribution drained = redistributeRunoff(slope, none, slope_storm);
    const StaticInundation empty = reconstructStaticInundation(slope, none, drained);
    assert(empty.status == InundationStatus::Available);
    assert(empty.depth_m.size() == 9);
    assert(empty.surfaces.empty());
    for (float depth : empty.depth_m) assert(depth == 0.0f);

    bool threw = false;
    try { (void)reconstructStaticInundation(single, single_basins, partial, 8); }
    catch (const std::runtime_error &) { threw = true; }
    assert(threw);
    std::atomic<bool> cancel{true};
    threw = false;
    try { (void)reconstructStaticInundation(single, single_basins, partial, 100, &cancel); }
    catch (const std::runtime_error &) { threw = true; }
    assert(threw);
    threw = false;
    ElevationGrid wrong = single;
    wrong.elevations_m[4] = std::numeric_limits<float>::quiet_NaN();
    try { (void)reconstructStaticInundation(wrong, single_basins, partial); }
    catch (const std::invalid_argument &) { threw = true; }
    assert(threw);
    return 0;
}
