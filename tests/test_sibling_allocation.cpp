#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/redistribution.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace Aowis::Hydrology;

namespace
{
ElevationGrid makeGrid(std::uint32_t columns, std::uint32_t rows,
                       const std::vector<float> &heights)
{
    ElevationGrid grid;
    grid.columns = columns;
    grid.rows = rows;
    grid.geometry.origin_x = 8.0;
    grid.geometry.origin_y = 50.0;
    grid.geometry.pixel_width = 0.001;
    grid.geometry.pixel_height = 0.001;
    grid.elevations_m = heights;
    return grid;
}

bool nearlyEqual(double left, double right)
{
    return std::abs(left - right) <= 1e-8 * std::max({1.0, std::abs(left), std::abs(right)});
}
}

int main()
{
    // Three independent 10m pits all connect for the FIRST time at one
    // 12m plateau. The surrounding boundary is 16m. Overflow from the
    // left pit can reach either dry receiving pit at the same saddle;
    // without a flow-rate law, which one receives it first is ambiguous.
    const ElevationGrid terrain = makeGrid(5, 5, {
        16,16,16,16,16,
        16,10,16,10,16,
        16,16,12,16,16,
        16,16,10,16,16,
        16,16,16,16,16
    });
    const DepressionHierarchy hierarchy = buildDepressionHierarchy(terrain);
    // Three physical leaf depressions connect at the single 12m saddle.
    assert(hierarchy.basins.size() == 4);
    const DepressionBasin &parent = hierarchy.basins.back();
    assert(parent.child_indices.size() == 3);
    const double capacity = hierarchy.basins.front().capacity_at_spill_m3;
    assert(capacity > 0.0);

    std::vector<double> runoff(terrain.elevations_m.size(), 0.0);
    runoff[6] = capacity * 1.4;
    const RunoffRedistribution ordinary = redistributeRunoff(terrain, hierarchy, runoff);
    const RunoffRedistribution ascending = redistributeRunoffWithSiblingOrder(
        terrain, hierarchy, runoff, SiblingAllocationOrder::BasinIndexAscending);
    const RunoffRedistribution descending = redistributeRunoffWithSiblingOrder(
        terrain, hierarchy, runoff, SiblingAllocationOrder::BasinIndexDescending);
    assert(nearlyEqual(ordinary.basin_storage_m3, ascending.basin_storage_m3));
    assert(nearlyEqual(ordinary.generated_runoff_m3, ascending.generated_runoff_m3));
    for (std::size_t i = 0; i < ordinary.basins.size(); ++i)
        assert(nearlyEqual(ordinary.basins[i].stored_in_subtree_m3,
                           ascending.basins[i].stored_in_subtree_m3));
    assert(nearlyEqual(ascending.basin_storage_m3, descending.basin_storage_m3));
    assert(nearlyEqual(ascending.total_boundary_outflow_m3, descending.total_boundary_outflow_m3));
    assert(nearlyEqual(ascending.basin_storage_m3, capacity * 1.4));
    assert(nearlyEqual(ascending.total_boundary_outflow_m3, 0.0));
    const SiblingAllocationSensitivity sensitivity = assessSiblingAllocationSensitivity(
        terrain, hierarchy, runoff);
    assert(sensitivity.allocation_sensitive);
    assert(sensitivity.differing_leaf_basins.size() == 2);
    assert(nearlyEqual(sensitivity.maximum_leaf_difference_m3, capacity * 0.4));
    assert(nearlyEqual(sensitivity.generated_runoff_m3, capacity * 1.4));
    assert(nearlyEqual(sensitivity.retained_storage_m3, capacity * 1.4));
    for (const SiblingAllocationDifference &difference : sensitivity.differing_leaf_basins)
    {
        assert(hierarchy.basins[std::size_t(difference.leaf_basin_index)].child_indices.empty());
        assert(nearlyEqual(difference.ascending_storage_m3,
            ascending.basins[std::size_t(difference.leaf_basin_index)].stored_in_subtree_m3));
        assert(nearlyEqual(difference.descending_storage_m3,
            descending.basins[std::size_t(difference.leaf_basin_index)].stored_in_subtree_m3));
    }

    // Optional capacity-weighted rule distributes the same 0.4 C overflow
    // among the two receiving basins according to their available capacities.
    // It must NOT depend on how child_indices happens to be enumerated.
    const RunoffRedistribution weighted = redistributeRunoffWithSiblingPolicy(
        terrain, hierarchy, runoff, SiblingAllocationPolicy::RemainingCapacityWeighted);
    const RunoffRedistribution weighted_legacy = redistributeRunoffWithSiblingPolicy(
        terrain, hierarchy, runoff, SiblingAllocationPolicy::IndexAscending);
    assert(nearlyEqual(weighted.generated_runoff_m3, ordinary.generated_runoff_m3));
    assert(nearlyEqual(weighted.basin_storage_m3, ordinary.basin_storage_m3));
    assert(nearlyEqual(weighted.total_boundary_outflow_m3, ordinary.total_boundary_outflow_m3));
    double receiving_capacity = 0.0;
    for (const std::uint64_t child : parent.child_indices)
        if (hierarchy.basins[std::size_t(child)].minimum_cell != 6)
            receiving_capacity += hierarchy.basins[std::size_t(child)].capacity_at_spill_m3;
    assert(receiving_capacity > 0.0);
    for (const std::uint64_t child : parent.child_indices)
    {
        const DepressionBasin &basin = hierarchy.basins[std::size_t(child)];
        const double actual = weighted.basins[std::size_t(child)].stored_in_subtree_m3;
        if (basin.minimum_cell == 6)
            assert(nearlyEqual(actual, basin.capacity_at_spill_m3));
        else
            assert(nearlyEqual(actual, capacity * 0.4 *
                basin.capacity_at_spill_m3 / receiving_capacity));
        assert(nearlyEqual(weighted_legacy.basins[std::size_t(child)].stored_in_subtree_m3,
                           ordinary.basins[std::size_t(child)].stored_in_subtree_m3));
    }
    DepressionHierarchy reordered = hierarchy;
    std::reverse(reordered.basins.back().child_indices.begin(),
                 reordered.basins.back().child_indices.end());
    const RunoffRedistribution weighted_reordered = redistributeRunoffWithSiblingPolicy(
        terrain, reordered, runoff, SiblingAllocationPolicy::RemainingCapacityWeighted);
    for (std::size_t i = 0; i < weighted.basins.size(); ++i)
        assert(nearlyEqual(weighted.basins[i].stored_in_subtree_m3,
                           weighted_reordered.basins[i].stored_in_subtree_m3));
    assert(nearlyEqual(weighted.mass_balance_error_m3, 0.0));

    // With enough water to fill every leaf, traversal order cannot change
    // any leaf volume. Agreement alone is not evidence of hydraulic truth.
    runoff[6] = parent.capacity_at_spill_m3 * 1.5;
    const SiblingAllocationSensitivity full = assessSiblingAllocationSensitivity(
        terrain, hierarchy, runoff);
    assert(!full.allocation_sensitive);
    assert(full.differing_leaf_basins.empty());
    assert(nearlyEqual(full.retained_storage_m3, parent.capacity_at_spill_m3));
    assert(nearlyEqual(full.boundary_outflow_m3, parent.capacity_at_spill_m3 * 0.5));

    // A solitary basin cannot have sibling-order sensitivity.
    const ElevationGrid single_pit = makeGrid(3, 3,
        {15,15,15, 15,10,15, 15,15,15});
    const DepressionHierarchy single = buildDepressionHierarchy(single_pit);
    std::vector<double> one_storm(9, 0.0);
    one_storm[4] = single.basins[0].capacity_at_spill_m3 * 0.5;
    const SiblingAllocationSensitivity isolated = assessSiblingAllocationSensitivity(
        single_pit, single, one_storm);
    assert(!isolated.allocation_sensitive);

    // Randomized regression: for varying terrain, both allocation orders
    // must conserve water and agree on every whole-domain mass-balance total.
    for (std::uint32_t seed = 1; seed <= 20; ++seed)
    {
        std::vector<float> heights(143, 35.0f);
        std::vector<double> random_runoff(143, 0.0);
        std::uint32_t value = seed * 2654435761U;
        for (std::uint32_t y = 1; y < 10; ++y)
            for (std::uint32_t x = 1; x < 12; ++x)
            {
                const std::size_t index = std::size_t(y) * 13 + x;
                value = value * 1664525U + 1013904223U;
                heights[index] = float(5 + ((value >> 22) % 29));
                value = value * 1664525U + 1013904223U;
                random_runoff[index] = double((value >> 14) % 200) * 37.0;
            }
        const ElevationGrid landscape = makeGrid(13, 11, heights);
        const DepressionHierarchy basins = buildDepressionHierarchy(landscape);
        const SiblingAllocationSensitivity comparison = assessSiblingAllocationSensitivity(
            landscape, basins, random_runoff);
        const RunoffRedistribution standard = redistributeRunoff(landscape, basins, random_runoff);
        assert(nearlyEqual(comparison.generated_runoff_m3, standard.generated_runoff_m3));
        assert(nearlyEqual(comparison.retained_storage_m3, standard.basin_storage_m3));
        assert(nearlyEqual(comparison.boundary_outflow_m3, standard.total_boundary_outflow_m3));
        assert(nearlyEqual(comparison.generated_runoff_m3,
                           comparison.retained_storage_m3 + comparison.boundary_outflow_m3));
        assert(comparison.allocation_sensitive == !comparison.differing_leaf_basins.empty());
        const RunoffRedistribution proportional = redistributeRunoffWithSiblingPolicy(
            landscape, basins, random_runoff,
            SiblingAllocationPolicy::RemainingCapacityWeighted);
        DepressionHierarchy altered = basins;
        for (DepressionBasin &basin : altered.basins)
            std::reverse(basin.child_indices.begin(), basin.child_indices.end());
        const RunoffRedistribution proportional_reordered = redistributeRunoffWithSiblingPolicy(
            landscape, altered, random_runoff,
            SiblingAllocationPolicy::RemainingCapacityWeighted);
        assert(nearlyEqual(proportional.generated_runoff_m3, standard.generated_runoff_m3));
        assert(nearlyEqual(proportional.basin_storage_m3, standard.basin_storage_m3));
        assert(nearlyEqual(proportional.total_boundary_outflow_m3,
                           standard.total_boundary_outflow_m3));
        for (std::size_t i = 0; i < basins.basins.size(); ++i)
            assert(nearlyEqual(proportional.basins[i].stored_in_subtree_m3,
                               proportional_reordered.basins[i].stored_in_subtree_m3));
    }

    std::atomic<bool> canceled{true};
    bool rejected = false;
    try { (void)assessSiblingAllocationSensitivity(terrain, hierarchy, runoff, &canceled); }
    catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { (void)redistributeRunoffWithSiblingOrder(terrain, hierarchy, runoff,
        static_cast<SiblingAllocationOrder>(99)); }
    catch (const std::invalid_argument &) { rejected = true; }
    rejected = false;
    try { (void)redistributeRunoffWithSiblingPolicy(terrain, hierarchy, runoff,
        static_cast<SiblingAllocationPolicy>(99)); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { (void)redistributeRunoffWithSiblingPolicy(terrain, hierarchy, runoff,
        SiblingAllocationPolicy::RemainingCapacityWeighted, &canceled); }
    catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
}
