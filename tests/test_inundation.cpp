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
    const StaticInundation separate_siblings = reconstructStaticInundation(paired, merged, incomplete);
    assert(separate_siblings.status == InundationStatus::Available);
    assert(separate_siblings.allocation_assumed);
    assert(separate_siblings.depth_m.size() == 35);
    assert(separate_siblings.surfaces.size() == 2);
    assert(separate_siblings.surfaces[0].basin_index != 2);
    assert(separate_siblings.surfaces[1].basin_index != 2);
    assert(separate_siblings.depth_m[15] > 0.0f);
    assert(separate_siblings.depth_m[19] > 0.0f);
    assert(separate_siblings.depth_m[15] != separate_siblings.depth_m[19]);
    assert(near(separate_siblings.reconstructed_volume_m3, incomplete.basin_storage_m3));

    // If a parent contains water that is neither held by its children nor
    // connected above their saddle, there is no defensible depth assignment.
    RunoffRedistribution inconsistent = incomplete;
    inconsistent.basins[2].stored_in_subtree_m3 += 100.0;
    inconsistent.basin_storage_m3 += 100.0;
    const StaticInundation unresolved = reconstructStaticInundation(paired, merged, inconsistent);
    assert(unresolved.status == InundationStatus::UnresolvedPartialMerge);
    assert(unresolved.depth_m.empty());
    assert(unresolved.surfaces.empty());

    merged_storm[15] = parent_capacity * 0.75;
    const RunoffRedistribution connected = redistributeRunoff(paired, merged, merged_storm);
    const StaticInundation merged_depth = reconstructStaticInundation(paired, merged, connected);
    assert(merged_depth.status == InundationStatus::Available);
    assert(!merged_depth.allocation_assumed);
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

    // Multi-level merge: two leaf ponds connect at 12 m, and that merged
    // child remains disconnected from a third 10 m pit until the 14 m saddle.
    // The active water surfaces are then one merged child plus one leaf,
    // rather than incorrectly treating the whole 18 m root as one pond.
    const ElevationGrid tiered = grid(7, 5, {
        18, 18, 18, 18, 18, 18, 18,
        18, 18, 18, 18, 18, 18, 18,
        18, 10, 12, 10, 14, 10, 18,
        18, 18, 18, 18, 18, 18, 18,
        18, 18, 18, 18, 18, 18, 18
    });
    const DepressionHierarchy tiered_tree = buildDepressionHierarchy(tiered);
    assert(tiered_tree.basins.size() == 5);
    std::uint64_t first_merged_index = tiered_tree.basins.size();
    std::uint64_t third_leaf_index = tiered_tree.basins.size();
    for (std::size_t i = 0; i < tiered_tree.basins.size(); ++i)
    {
        const DepressionBasin &basin = tiered_tree.basins[i];
        if (near(basin.formation_elevation_m, 12.0) &&
            basin.child_indices.size() == 2)
            first_merged_index = i;
        if (basin.child_indices.empty() && basin.minimum_cell == 19)
            third_leaf_index = i;
    }
    assert(first_merged_index < tiered_tree.basins.size());
    assert(third_leaf_index < tiered_tree.basins.size());
    std::vector<double> tiered_storm(35, 0.0);
    tiered_storm[15] =
        tiered_tree.basins[std::size_t(first_merged_index)].capacity_at_spill_m3 +
        0.5 * tiered_tree.basins[std::size_t(third_leaf_index)].capacity_at_spill_m3;
    const RunoffRedistribution tiered_runoff =
        redistributeRunoff(tiered, tiered_tree, tiered_storm);
    const StaticInundation tiered_depth = reconstructStaticInundation(
        tiered, tiered_tree, tiered_runoff);
    assert(tiered_depth.status == InundationStatus::Available);
    assert(tiered_depth.allocation_assumed);
    assert(tiered_depth.surfaces.size() == 2);
    assert(tiered_depth.depth_m[15] > 0.0f);
    assert(tiered_depth.depth_m[17] > 0.0f);
    assert(tiered_depth.depth_m[19] > 0.0f);
    assert(near(tiered_depth.reconstructed_volume_m3, tiered_runoff.basin_storage_m3));

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
    assert(!independent_depth.allocation_assumed);
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
