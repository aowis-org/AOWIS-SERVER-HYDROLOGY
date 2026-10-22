#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/redistribution.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <stdexcept>
#include <limits>
#include <vector>

using namespace Aowis::Hydrology;

namespace
{
ElevationGrid grid(std::uint32_t columns, std::uint32_t rows,
                   const std::vector<float> &values)
{
    ElevationGrid result;
    result.columns = columns;
    result.rows = rows;
    result.geometry.origin_x = 8.0;
    result.geometry.origin_y = 50.0;
    result.geometry.pixel_width = 0.001;
    result.geometry.pixel_height = 0.001;
    result.geometry.vertical_datum = VerticalDatum::Egm2008;
    result.elevations_m = values;
    return result;
}

bool equal(double left, double right)
{
    return std::abs(left - right) < 1e-7 * std::max(1.0, std::max(std::abs(left), std::abs(right)));
}

void balanced(const RunoffRedistribution &result)
{
    assert(equal(result.generated_runoff_m3, result.basin_storage_m3 + result.total_boundary_outflow_m3));
    assert(equal(result.mass_balance_error_m3, 0.0));
}
}

int main()
{
    const ElevationGrid pit = grid(3, 3, {
        15, 15, 15,
        15, 10, 15,
        15, 15, 15
    });
    const DepressionHierarchy single = buildDepressionHierarchy(pit);
    const double capacity = single.basins[0].capacity_at_spill_m3;
    assert(capacity > 0.0);
    std::vector<double> volumes(9, 0.0);
    volumes[4] = capacity * 0.4;
    const RunoffRedistribution partially_filled = redistributeRunoff(pit, single, volumes);
    balanced(partially_filled);
    assert(equal(partially_filled.basin_storage_m3, capacity * 0.4));
    assert(equal(partially_filled.total_boundary_outflow_m3, 0.0));
    assert(!partially_filled.basins[0].at_spill);

    volumes[4] = capacity * 1.4;
    const RunoffRedistribution overflowing = redistributeRunoff(pit, single, volumes);
    balanced(overflowing);
    assert(equal(overflowing.basin_storage_m3, capacity));
    assert(equal(overflowing.basin_boundary_outflow_m3, capacity * 0.4));
    assert(overflowing.basins[0].at_spill);

    const ElevationGrid slope = grid(3, 3, {
        10, 10, 10,
        10, 12, 10,
        10, 10, 10
    });
    const DepressionHierarchy no_basins = buildDepressionHierarchy(slope);
    assert(no_basins.basins.empty());
    volumes.assign(9, 0.0);
    volumes[4] = 12.5;
    const RunoffRedistribution direct = redistributeRunoff(slope, no_basins, volumes);
    balanced(direct);
    assert(equal(direct.direct_boundary_outflow_m3, 12.5));
    assert(equal(direct.basin_storage_m3, 0.0));

    // Water from the left leaf first fills it to its 12m saddle, then moves
    // to the right leaf, finally raising the shared parent basin to 16m.
    const ElevationGrid paired = grid(7, 5, {
        16, 16, 16, 16, 16, 16, 16,
        16, 16, 16, 16, 16, 16, 16,
        16, 10, 12, 12, 12, 10, 16,
        16, 16, 16, 16, 16, 16, 16,
        16, 16, 16, 16, 16, 16, 16
    });
    const DepressionHierarchy merged = buildDepressionHierarchy(paired);
    assert(merged.basins.size() == 3);
    std::vector<double> paired_volumes(35, 0.0);
    const double leaf_capacity = merged.basins[0].capacity_at_spill_m3;
    const double parent_capacity = merged.basins[2].capacity_at_spill_m3;
    paired_volumes[15] = leaf_capacity * 1.5;
    const RunoffRedistribution shared = redistributeRunoff(paired, merged, paired_volumes);
    balanced(shared);
    assert(equal(shared.basin_storage_m3, leaf_capacity * 1.5));
    assert(equal(shared.basins[0].stored_in_subtree_m3, leaf_capacity));
    assert(equal(shared.basins[1].stored_in_subtree_m3, leaf_capacity * 0.5));
    assert(equal(shared.basins[2].stored_in_subtree_m3, leaf_capacity * 1.5));
    assert(equal(shared.total_boundary_outflow_m3, 0.0));

    paired_volumes[15] = parent_capacity * 1.2;
    const RunoffRedistribution above_parent = redistributeRunoff(paired, merged, paired_volumes);
    balanced(above_parent);
    assert(equal(above_parent.basin_storage_m3, parent_capacity));
    assert(equal(above_parent.basin_boundary_outflow_m3, parent_capacity * 0.2));
    assert(equal(above_parent.basins[0].stored_in_subtree_m3, leaf_capacity));
    assert(equal(above_parent.basins[1].stored_in_subtree_m3, leaf_capacity));
    assert(above_parent.basins[2].at_spill);

    // Routing settings on an independent analysis must not change the event
    // result: the original DEM exclusively defines both paths and storage.
    AnalysisOptions fill;
    fill.fill_depressions = true;
    const AnalysisResult conditioned = analyze(paired, fill);
    assert(conditioned.conditioned_elevations_m != paired.elevations_m);
    const RunoffRedistribution repeat = redistributeRunoff(paired, merged, paired_volumes);
    assert(equal(repeat.basin_storage_m3, above_parent.basin_storage_m3));

    // Random landscapes and storm volumes check strict conservation and
    // monotonic root storage (doubling rainfall must not reduce storage).
    for (std::uint32_t seed = 1; seed <= 36; ++seed)
    {
        ElevationGrid landscape = grid(13, 11, std::vector<float>(143, 50.0f));
        std::vector<double> storm(143, 0.0);
        std::uint32_t random = seed * 2654435761U;
        for (std::uint32_t row = 1; row < 10; ++row)
            for (std::uint32_t column = 1; column < 12; ++column)
            {
                const std::size_t index = std::size_t(row) * 13 + column;
                random = random * 1664525U + 1013904223U;
                landscape.elevations_m[index] = float(5 + (random >> 24) % 25);
                storm[index] = double((random >> 13) % 500) * 250.0;
            }
        const DepressionHierarchy basins = buildDepressionHierarchy(landscape);
        const RunoffRedistribution one_storm = redistributeRunoff(landscape, basins, storm);
        balanced(one_storm);
        for (double &amount : storm) amount *= 2.0;
        const RunoffRedistribution doubled = redistributeRunoff(landscape, basins, storm);
        balanced(doubled);
        assert(doubled.basin_storage_m3 + 0.001 >= one_storm.basin_storage_m3);
    }

    std::atomic<bool> canceled{true};
    bool rejected = false;
    try { (void)redistributeRunoff(paired, merged, paired_volumes, &canceled); }
    catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    rejected = false;
    paired_volumes[15] = -1;
    try { (void)redistributeRunoff(paired, merged, paired_volumes); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    rejected = false;
    paired_volumes[15] = 1;
    ElevationGrid missing = paired;
    missing.elevations_m[9] = std::numeric_limits<float>::quiet_NaN();
    try { (void)redistributeRunoff(missing, merged, paired_volumes); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    return 0;
}
