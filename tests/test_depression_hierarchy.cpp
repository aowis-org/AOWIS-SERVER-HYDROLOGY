#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/depression_hierarchy.h>
#include <aowis/hydrology/depression_archive.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <atomic>
#include <queue>

using namespace Aowis::Hydrology;

namespace
{
ElevationGrid gridOf(std::uint32_t columns, std::uint32_t rows,
                     const std::vector<float> &heights)
{
    ElevationGrid grid;
    grid.columns = columns;
    grid.rows = rows;
    grid.geometry.origin_x = 8.0;
    grid.geometry.origin_y = 50.0;
    grid.geometry.pixel_width = 0.001;
    grid.geometry.pixel_height = 0.001;
    grid.geometry.vertical_datum = VerticalDatum::Egm2008;
    grid.elevations_m = heights;
    return grid;
}

bool approximately(double left, double right, double tolerance = 1e-8)
{
    return std::abs(left - right) <= tolerance * std::max({1.0, std::abs(left), std::abs(right)});
}
}

int main()
{
    // A 3x3 square pit with an open rim: one basin, capacity 5 metres * its
    // (geographic/ellipsoidal) single-cell area, spilling over the rim at 15m.
    const ElevationGrid single = gridOf(3, 3, {
        15, 15, 15,
        15, 10, 15,
        15, 15, 15
    });
    const DepressionHierarchy one = buildDepressionHierarchy(single);
    assert(one.basins.size() == 1);
    const DepressionBasin &pit = one.basins.front();
    assert(pit.minimum_cell == 4);
    assert(pit.minimum_elevation_m == 10);
    assert(pit.spill_elevation_m == 15);
    assert(pit.parent_index == -1);
    assert(pit.spills_to_domain_boundary);
    const double area = rasterRowCellAreaM2(single.geometry, single.columns, 1);
    assert(approximately(pit.capacity_at_spill_m3, 5.0 * area));
    const BasinStorageState midway = basinStorageAtLevel(single, one, 0, 12);
    assert(midway.inundated_cells == 1);
    assert(approximately(midway.water_volume_m3, 2.0 * area));
    const BasinStorageState full = basinStorageAtLevel(single, one, 0, 15);
    assert(approximately(full.water_volume_m3, pit.capacity_at_spill_m3));
    assert(full.inundated_cells == 1);
    bool rejected = false;
    try { (void)basinStorageAtLevel(single, one, 0, 16); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);

    // Every elevation is distinct: the unique-height fast path must still
    // discover the same connected basin and calculate its spill volume.
    // Large groups of single-cell elevations must not need plateau maps.
    const ElevationGrid unique_heights = gridOf(5, 5, {
        40, 41, 42, 43, 44,
        45, 14, 15, 16, 46,
        47, 17, 10, 18, 48,
        49, 19, 11, 13, 50,
        51, 52, 53, 54, 55
    });
    const DepressionHierarchy unique_hierarchy = buildDepressionHierarchy(unique_heights);
    assert(unique_hierarchy.basins.size() == 1);
    const DepressionBasin &unique_basin = unique_hierarchy.basins.front();
    assert(unique_basin.minimum_cell == 12);
    assert(unique_basin.spill_elevation_m == 40);
    assert(unique_basin.spills_to_domain_boundary);
    assert(unique_basin.cells_below_spill == 9);
    double unique_volume = 0.0;
    for (std::uint32_t row = 1; row < 4; ++row)
        for (std::uint32_t col = 1; col < 4; ++col)
            unique_volume += (40.0 - unique_heights.elevations_m[std::size_t(row * 5 + col)]) *
                rasterRowCellAreaM2(unique_heights.geometry, unique_heights.columns, row);
    assert(approximately(unique_basin.capacity_at_spill_m3, unique_volume));

    // Two minima in a broad valley connect across a 12-metre saddle, then
    // spill through a 16-metre rim. Both leaves must have the merged basin
    // as parent. Each leaf has 2 metres of capacity when first joining.
    const ElevationGrid paired = gridOf(7, 5, {
        16, 16, 16, 16, 16, 16, 16,
        16, 16, 16, 16, 16, 16, 16,
        16, 10, 12, 12, 12, 10, 16,
        16, 16, 16, 16, 16, 16, 16,
        16, 16, 16, 16, 16, 16, 16
    });
    const DepressionHierarchy nested = buildDepressionHierarchy(paired);
    assert(nested.basins.size() == 3);
    const DepressionBasin &left = nested.basins[0];
    const DepressionBasin &right = nested.basins[1];
    const DepressionBasin &parent = nested.basins[2];
    assert(left.parent_index == 2 && right.parent_index == 2);
    assert(left.spill_elevation_m == 12 && right.spill_elevation_m == 12);
    assert(!left.spills_to_domain_boundary && !right.spills_to_domain_boundary);
    assert(parent.child_indices.size() == 2);
    assert(parent.minimum_elevation_m == 10 && parent.spill_elevation_m == 16);
    assert(parent.spills_to_domain_boundary);
    const double left_area = rasterRowCellAreaM2(paired.geometry, paired.columns, 2);
    assert(approximately(left.capacity_at_spill_m3, 2.0 * left_area));
    assert(approximately(right.capacity_at_spill_m3, 2.0 * left_area));
    // At 13 m the connected pond covers both 10m bottoms plus three 12m
    // saddle pixels, and a 5-cell connected component.
    const BasinStorageState merged_level = basinStorageAtLevel(paired, nested, 2, 13);
    assert(merged_level.inundated_cells == 5);
    assert(approximately(merged_level.water_volume_m3, 2 * 3.0 * left_area + 3 * left_area));
    // The domain-rim spill capacity counts two pit cells (6m each) and three
    // saddle cells (4m each); saddle-height rim cells contribute zero.
    assert(approximately(parent.capacity_at_spill_m3, 24 * left_area));

    assert(parent.formation_elevation_m == 12);
    assert(left.formation_elevation_m == 10);
    rejected = false;
    try { (void)basinStorageAtLevel(paired, nested, 2, 11); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);

    // Independently verify every finite basin capacity on many synthetic
    // landscapes. A connected component below the spill saddle must store
    // exactly the water represented by its own cells; equal-saddle merges can
    // have multiple disconnected pre-spill components and are tested above.
    for (std::uint32_t seed = 1; seed <= 30; ++seed)
    {
        ElevationGrid landscape = gridOf(13, 11, std::vector<float>(143, 50.0f));
        std::uint32_t state = seed * 747796405U + 2891336453U;
        for (std::uint32_t y = 1; y < 10; ++y)
            for (std::uint32_t x = 1; x < 12; ++x)
            {
                state = state * 1664525U + 1013904223U;
                landscape.elevations_m[std::size_t(y) * 13 + x] =
                    float(10 + (state >> 19) % 35);
            }
        const DepressionHierarchy sample = buildDepressionHierarchy(landscape);
        for (const DepressionBasin &basin : sample.basins)
        {
            if (!std::isfinite(basin.spill_elevation_m) ||
                basin.spill_elevation_m == basin.formation_elevation_m)
                continue;
            const double level = basin.spill_elevation_m;
            std::vector<std::uint8_t> explored(143, 0);
            std::queue<std::uint64_t> cells;
            explored[std::size_t(basin.minimum_cell)] = 1;
            cells.push(basin.minimum_cell);
            double expected = 0.0;
            while (!cells.empty())
            {
                const std::uint64_t index = cells.front();
                cells.pop();
                const double elevation = landscape.elevations_m[std::size_t(index)];
                expected += (level - elevation) * rasterRowCellAreaM2(
                    landscape.geometry, landscape.columns, std::uint32_t(index / 13));
                const std::int64_t xx = std::int64_t(index % 13);
                const std::int64_t yy = std::int64_t(index / 13);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        if (dx == 0 && dy == 0) continue;
                        const std::int64_t nx = xx + dx;
                        const std::int64_t ny = yy + dy;
                        if (nx < 0 || nx >= 13 || ny < 0 || ny >= 11) continue;
                        const std::uint64_t other = std::uint64_t(ny) * 13 + std::uint64_t(nx);
                        if (explored[std::size_t(other)] ||
                            landscape.elevations_m[std::size_t(other)] >= level) continue;
                        explored[std::size_t(other)] = 1;
                        cells.push(other);
                    }
            }
            assert(approximately(expected, basin.capacity_at_spill_m3, 1e-7));
        }
    }

    // Reusing each raster row's metric area must not flatten geographic or
    // projection-induced area changes. Separate pits at different rows each
    // have a known one-cell pre-spill volume, and storage queries use the
    // same exact geodesic area as hierarchy construction.
    ElevationGrid row_pits = gridOf(5, 7, std::vector<float>(35, 20.0f));
    row_pits.elevations_m[7] = 10.0f;  // row 1, column 2
    row_pits.elevations_m[27] = 14.0f; // row 5, column 2
    for (int projection = 0; projection < 2; ++projection)
    {
        ElevationGrid test_grid = row_pits;
        if (projection == 1)
        {
            test_grid.geometry.horizontal_crs = HorizontalCrs::Wgs84Utm;
            test_grid.geometry.utm_zone = 32;
            test_grid.geometry.northern_hemisphere = true;
            test_grid.geometry.origin_x = 500000.0;
            test_grid.geometry.origin_y = 5700000.0;
            test_grid.geometry.pixel_width = 30.0;
            test_grid.geometry.pixel_height = 30.0;
        }
        const DepressionHierarchy hierarchy = buildDepressionHierarchy(test_grid);
        assert(hierarchy.basins.size() == 2);
        for (const DepressionBasin &basin : hierarchy.basins)
        {
            const std::uint32_t row = std::uint32_t(basin.minimum_cell / test_grid.columns);
            const double area_m2 = rasterRowCellAreaM2(
                test_grid.geometry, test_grid.columns, row);
            assert(basin.spill_elevation_m == 20.0);
            assert(basin.cells_below_spill == 1);
            assert(approximately(basin.capacity_at_spill_m3,
                                 (20.0 - basin.minimum_elevation_m) * area_m2));
            const double level = 0.5 * (20.0 + basin.minimum_elevation_m);
            const BasinStorageState sampled = basinStorageAtLevel(
                test_grid, hierarchy, basin.index, level);
            assert(sampled.inundated_cells == 1);
            assert(approximately(sampled.water_volume_m3,
                                 (level - basin.minimum_elevation_m) * area_m2));
        }
    }

    // Plateaus must not spawn one basin per pixel.
    const ElevationGrid flat = gridOf(5, 5, {
        20, 20, 20, 20, 20,
        20, 10, 10, 10, 20,
        20, 10, 10, 10, 20,
        20, 10, 10, 10, 20,
        20, 20, 20, 20, 20
    });
    const DepressionHierarchy plateau = buildDepressionHierarchy(flat);
    assert(plateau.basins.size() == 1);
    assert(plateau.basins[0].spill_elevation_m == 20);
    assert(plateau.basins[0].capacity_at_spill_m3 > 0);

    // Strict coverage detects interior holes; optional open-hole mode treats
    // a missing cell as an open outlet rather than fabricating elevation.
    ElevationGrid missing = flat;
    missing.elevations_m[12] = std::numeric_limits<float>::quiet_NaN();
    rejected = false;
    try { (void)buildDepressionHierarchy(missing); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    const DepressionHierarchy open_hole = buildDepressionHierarchy(missing, false);
    assert(open_hole.has_open_boundary);

    // The source DEM and physical storage must not change with D8
    // breaching/filling settings (this builder never consumes routing data).
    const std::vector<float> copy = paired.elevations_m;
    const AnalysisResult conditioned = analyze(paired);
    assert(conditioned.source.elevations_m == paired.elevations_m);
    assert(buildDepressionHierarchy(conditioned.source).basins.size() == nested.basins.size());
    assert(paired.elevations_m == copy);

    // Persisted hierarchy is byte-stable and retains the exact hierarchy,
    // double-precision capacities and reconstructed child relationships.
    const std::vector<std::uint8_t> archive = encodeDepressionHierarchy(nested);
    const DepressionHierarchy restored = decodeDepressionHierarchy(archive, 7, 5);
    assert(restored.basins.size() == nested.basins.size());
    assert(restored.basins[2].child_indices.size() == 2);
    assert(restored.basins[0].parent_index == 2);
    assert(approximately(restored.basins[2].capacity_at_spill_m3,
                         nested.basins[2].capacity_at_spill_m3));
    assert(archive == encodeDepressionHierarchy(restored));
    std::vector<std::uint8_t> truncated = archive;
    truncated.pop_back();
    rejected = false;
    try { (void)decodeDepressionHierarchy(truncated, 7, 5); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    std::vector<std::uint8_t> corrupted = archive;
    corrupted[50] ^= 1U;
    rejected = false;
    try { (void)decodeDepressionHierarchy(corrupted, 7, 5); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { (void)decodeDepressionHierarchy(archive, 8, 5); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    const DepressionHierarchy empty_hierarchy = buildDepressionHierarchy(gridOf(3,3, {
       5,5,5, 5,5,5, 5,5,5
    }));
    const std::vector<std::uint8_t> empty_archive = encodeDepressionHierarchy(empty_hierarchy);
    assert(decodeDepressionHierarchy(empty_archive, 3, 3).basins.empty());

    // Resource limits and cancellation are explicit failures.
    rejected = false;
    try { (void)basinStorageAtLevel(paired, nested, 2, 13, 2); }
    catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    std::atomic<bool> cancelled(true);
    rejected = false;
    try { (void)buildDepressionHierarchy(paired, true, &cancelled); }
    catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
}
