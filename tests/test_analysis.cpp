#include <aowis/hydrology/analysis.h>

#ifdef NDEBUG
#undef NDEBUG // Exercise assertions in Release builds, too.
#endif
#include <cassert>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace Aowis::Hydrology;

int main()
{
    ElevationGrid grid;
    grid.columns = 5;
    grid.rows = 5;
    grid.geometry.origin_x = 8.0;
    grid.geometry.origin_y = 50.0;
    grid.geometry.pixel_width = 0.001;
    grid.geometry.pixel_height = 0.001;
    grid.elevations_m = {
        18, 17, 16, 15, 14,
        17, 16, 15, 14, 13,
        16, 15,  1, 13, 12,
        15, 14, 13, 12, 11,
        14, 13, 12, 11, 10
    };
    // GeographicLib WGS84 ellipsoid metrics: a 0.001° cell near 50°N
    // covers approximately 7,970 m² (not 0.001² or an assumed 900 m²).
    grid.geometry.vertical_datum = VerticalDatum::Egm2008;
    const double geographic_area = rasterRowCellAreaM2(grid.geometry, grid.columns, 0);
    assert(geographic_area > 7800.0 && geographic_area < 8100.0);
    const double geographic_east = rasterRowNeighborDistanceM(
        grid.geometry, grid.columns, 0, 1, 0);
    const double geographic_south = rasterRowNeighborDistanceM(
        grid.geometry, grid.columns, 0, 0, 1);
    assert(geographic_east > 70.0 && geographic_east < 73.0);
    assert(geographic_south > 110.0 && geographic_south < 113.0);

    // Projected GLO-30 alternatives and future UTM LiDAR grids must work
    // without converting elevations into the geographic source raster.
    const UtmCoordinate utm = wgs84ToUtm(8.5, 50.5);
    assert(utm.zone == 32 && utm.north);
    const MapCoordinate reverse = utmToWgs84(utm);
    assert(std::abs(reverse.x - 8.5) < 1e-9);
    assert(std::abs(reverse.y - 50.5) < 1e-9);
    const UtmCoordinate south_utm = wgs84ToUtm(18.42, -33.93);
    assert(south_utm.zone == 34 && !south_utm.north);
    const MapCoordinate south_reversed = utmToWgs84(south_utm);
    assert(std::abs(south_reversed.x - 18.42) < 1e-9);
    assert(std::abs(south_reversed.y + 33.93) < 1e-9);
    ElevationGrid projected = grid;
    projected.geometry.horizontal_crs = HorizontalCrs::Wgs84Utm;
    projected.geometry.utm_zone = utm.zone;
    projected.geometry.northern_hemisphere = utm.north;
    projected.geometry.origin_x = utm.easting_m - 50.0;
    projected.geometry.origin_y = utm.northing_m + 50.0;
    projected.geometry.pixel_width = 25.0;
    projected.geometry.pixel_height = 25.0;
    const MapCoordinate local_pixel = pixelCentre(projected.geometry, 0, 0);
    const MapCoordinate geographic_pixel = pixelCentreWgs84(projected.geometry, 0, 0);
    const MapCoordinate pixel_again = utmToWgs84({utm.zone, utm.north,
        local_pixel.x, local_pixel.y});
    assert(std::abs(geographic_pixel.x - pixel_again.x) < 1e-12);
    assert(std::abs(geographic_pixel.y - pixel_again.y) < 1e-12);
    const double projected_area = rasterRowCellAreaM2(
        projected.geometry, projected.columns, 0);
    assert(projected_area > 620.0 && projected_area < 630.0);
    AnalysisResult projected_result = analyze(projected);
    assert(projected_result.conditioned_cell_count > 0);
    assert(projected_result.upstream_cell_count[24] >= 1);
    assert(std::abs(projected_result.upstream_area_m2[24] /
        projected_result.upstream_cell_count[24] - projected_area) < 2.0);
    RasterGeometry invalid_projection = projected.geometry;
    invalid_projection.utm_zone = 0;
    bool bad_crs_rejected = false;
    try { validateRasterGeometry(invalid_projection, 5, 5); }
    catch (const std::invalid_argument &) { bad_crs_rejected = true; }
    assert(bad_crs_rejected);
    AnalysisResult result = analyze(grid);
    assert(result.source.elevations_m[12] == 1);
    assert(result.conditioned_elevations_m[12] > 1.0f);
    assert(result.conditioned_elevations_m[12] == 12.0f);
    assert(result.potential_fill_depth_m[12] == 11.0f);
    assert(result.maximum_fill_m == 11.0f);
    assert(result.potential_fill_volume_m3 > 0.0);
    assert(result.potential_fill_cell_count == result.conditioned_cell_count);
    assert(result.conditioned_cell_count > 0);
    assert(result.upstream_cell_count[12] >= 1);
    assert(result.downstream[12] >= 0);
    assert(result.upstream_area_m2[12] > 0.0);
    assert(result.downstream[24] == -1);
    const std::vector<std::uint8_t> catchment = delineateCatchment(result, 24);
    assert(catchment[24] == 1);
    assert(result.upstream_cell_count[24] >= 1);
    std::uint64_t counted = 0;
    for (std::uint8_t part : catchment)
        counted += part;
    assert(counted == result.upstream_cell_count[24]);

    // A simple slope flows into its lower-right boundary outlets.
    ElevationGrid slope = grid;
    for (std::uint32_t row = 0; row < grid.rows; ++row)
        for (std::uint32_t col = 0; col < grid.columns; ++col)
            slope.elevations_m[row * grid.columns + col] =
                float(100 - row * 10 - col * 10);
    AnalysisResult slope_result = analyze(slope);
    assert(slope_result.conditioned_cell_count == 0);
    assert(slope_result.downstream[12] == 18);
    assert(slope_result.downstream[24] == -1);
    assert(slope_result.upstream_cell_count[24] > 1);
    const std::vector<std::uint8_t> slope_catchment = delineateCatchment(slope_result, 24);
    std::uint64_t slope_count = 0;
    for (std::uint8_t part : slope_catchment) slope_count += part;
    assert(slope_result.upstream_cell_count[24] == slope_count);

    // The no-fill mode must preserve a true closed depression/sink.
    AnalysisOptions no_fill;
    no_fill.fill_depressions = false;
    AnalysisResult unfilled = analyze(grid, no_fill);
    assert(unfilled.downstream[12] == -1);
    assert(unfilled.conditioned_elevations_m[12] == 1.0f);
    assert(unfilled.potential_fill_depth_m[12] == 11.0f);
    assert(unfilled.potential_fill_cell_count == result.potential_fill_cell_count);
    assert(unfilled.potential_fill_volume_m3 == result.potential_fill_volume_m3);
    assert(unfilled.conditioned_cell_count == 0);
    bool rejected_hole = false;
    ElevationGrid missing = grid;
    missing.elevations_m[12] = std::numeric_limits<float>::quiet_NaN();
    try { (void)analyze(missing); }
    catch (const std::invalid_argument &) { rejected_hole = true; }
    assert(rejected_hole);

    // Completely flat terrain must never create cyclic or disconnected
    // flow after conditioning; every interior cell has a downstream path.
    ElevationGrid flat = grid;
    flat.elevations_m.assign(25, 10.0f);
    AnalysisResult flat_result = analyze(flat);
    for (std::uint64_t index = 0; index < 25; ++index)
    {
        assert(flat_result.conditioned_elevations_m[std::size_t(index)] == 10.0f);
        assert(flat_result.potential_fill_depth_m[std::size_t(index)] == 0.0f);
        if (index / 5 > 0 && index / 5 < 4 && index % 5 > 0 && index % 5 < 4)
            assert(flat_result.downstream[std::size_t(index)] >= 0);
    }
    // In a flat closed bowl, preserve the physical bottom and exactly one
    // terminal sink. Equal-elevation connections must not form graph cycles.
    ElevationGrid bowl = grid;
    bowl.elevations_m = {
        8, 8, 8, 8, 8,
        8, 2, 2, 2, 8,
        8, 2, 2, 2, 8,
        8, 2, 2, 2, 8,
        8, 8, 8, 8, 8
    };
    AnalysisResult basin = analyze(bowl, no_fill);
    std::uint64_t closed_sinks = 0;
    for (std::uint64_t index = 0; index < 25; ++index)
    {
        if (bowl.elevations_m[std::size_t(index)] == 2.0f &&
            basin.downstream[std::size_t(index)] == -1)
        {
            ++closed_sinks;
            assert(basin.upstream_cell_count[std::size_t(index)] == 25);
        }
        assert(basin.conditioned_elevations_m[std::size_t(index)] ==
               bowl.elevations_m[std::size_t(index)]);
    }
    assert(closed_sinks == 1);
    const AnalysisResult filled_basin = analyze(bowl);
    for (std::uint64_t index = 0; index < 25; ++index)
    {
        const float expected = bowl.elevations_m[std::size_t(index)] == 2.0f ? 6.0f : 0.0f;
        assert(filled_basin.potential_fill_depth_m[std::size_t(index)] == expected);
        assert(filled_basin.conditioned_elevations_m[std::size_t(index)] == 8.0f);
    }
    double expected_volume = 0.0;
    for (std::uint32_t y = 1; y <= 3; ++y)
        expected_volume += 6.0 * 3.0 * rasterRowCellAreaM2(bowl.geometry, bowl.columns, y);
    assert(std::abs(filled_basin.potential_fill_volume_m3 - expected_volume) < 1e-6);
    assert(filled_basin.potential_fill_cell_count == 9);
    assert(basin.potential_fill_volume_m3 == filled_basin.potential_fill_volume_m3);
    // Conservative breach of a one-cell artificial embankment. Water can
    // escape through a strictly lower, preexisting downstream channel, but
    // without the new option the deep pit must remain a closed sink.
    ElevationGrid embankment = grid;
    embankment.columns = 9;
    embankment.rows = 7;
    embankment.elevations_m.assign(63, 15.0f);
    embankment.elevations_m[3 * 9 + 3] = 4.0f;
    embankment.elevations_m[3 * 9 + 4] = 10.0f;
    embankment.elevations_m[3 * 9 + 5] = 3.0f;
    embankment.elevations_m[3 * 9 + 6] = 2.0f;
    embankment.elevations_m[3 * 9 + 7] = 1.0f;
    embankment.elevations_m[3 * 9 + 8] = 0.0f;
    const AnalysisResult retained = analyze(embankment, no_fill);
    assert(retained.downstream[3 * 9 + 3] == -1);
    AnalysisOptions constrained = no_fill;
    constrained.breach_depressions = true;
    constrained.maximum_breach_cut_depth_m = 6.0;
    constrained.maximum_breach_path_cells = 4;
    const AnalysisResult breached = analyze(embankment, constrained);
    assert(breached.source.elevations_m[3 * 9 + 4] == 10.0f);
    assert(breached.conditioned_elevations_m[3 * 9 + 4] == 4.0f);
    assert(breached.breach_cut_depth_m[3 * 9 + 4] == 6.0f);
    assert(breached.accepted_breach_paths == 1);
    assert(breached.breached_cell_count == 1);
    assert(breached.conditioned_cell_count == 0);
    assert(breached.downstream[3 * 9 + 3] >= 0);
    const double expected_excavation = 6.0 * rasterRowCellAreaM2(
        embankment.geometry, embankment.columns, 3);
    assert(std::abs(breached.breach_excavation_m3 - expected_excavation) < 1e-5);
    assert(breached.maximum_breach_cut_m == 6.0f);
    assert(std::abs(breached.potential_fill_volume_m3 -
                    retained.potential_fill_volume_m3) < 1e-6);
    // No excavations when no plausible route satisfies the user's limits.
    AnalysisOptions strict = constrained;
    strict.maximum_breach_cut_depth_m = 5.0;
    const AnalysisResult rejected = analyze(embankment, strict);
    assert(rejected.accepted_breach_paths == 0);
    assert(rejected.rejected_breach_candidates >= 1);
    assert(rejected.downstream[3 * 9 + 3] == -1);
    assert(rejected.breached_cell_count == 0);
    AnalysisOptions volume_limited = constrained;
    volume_limited.maximum_breach_excavation_m3 = 1.0;
    const AnalysisResult volume_rejected = analyze(embankment, volume_limited);
    assert(volume_rejected.accepted_breach_paths == 0);
    assert(volume_rejected.breached_cell_count == 0);
    AnalysisOptions length_limited = constrained;
    length_limited.maximum_breach_path_cells = 2;
    // One obstruction followed by an outlet is exactly two steps.
    const AnalysisResult length_result = analyze(embankment, length_limited);
    assert(length_result.accepted_breach_paths == 1);
    // Breaching may be followed by filling any remaining closed depressions.
    AnalysisOptions breach_and_fill = constrained;
    breach_and_fill.fill_depressions = true;
    const AnalysisResult combined = analyze(embankment, breach_and_fill);
    assert(combined.breach_cut_depth_m[3 * 9 + 4] == 6.0f);
    assert(combined.conditioned_elevations_m[3 * 9 + 4] == 4.0f);
    // Invalid constraints must fail explicitly, not silently fall back.
    AnalysisOptions invalid_breach = constrained;
    invalid_breach.maximum_breach_path_cells = 33;
    bool invalid_breach_rejected = false;
    try { (void)analyze(embankment, invalid_breach); }
    catch (const std::invalid_argument &) { invalid_breach_rejected = true; }
    assert(invalid_breach_rejected);

    // Nested basins: a shallow internal shelf and deeper pocket must retain
    // their physical elevations, even when the fill algorithm reports a
    // single boundary-connected spill surface.
    ElevationGrid nested = bowl;
    nested.elevations_m[12] = 0.0f;
    nested.elevations_m[13] = 1.0f;
    const AnalysisResult nested_fill = analyze(nested);
    assert(nested_fill.conditioned_elevations_m[12] == 8.0f);
    assert(nested_fill.potential_fill_depth_m[12] == 8.0f);
    assert(nested_fill.potential_fill_depth_m[13] == 7.0f);
    assert(nested_fill.source.elevations_m[12] == 0.0f);
    const AnalysisResult nested_preserved = analyze(nested, no_fill);
    assert(nested_preserved.conditioned_elevations_m[12] == 0.0f);
    assert(nested_preserved.potential_fill_volume_m3 ==
           nested_fill.potential_fill_volume_m3);
    ElevationGrid multiple_pits = bowl;
    multiple_pits.elevations_m[6] = 0.0f;
    multiple_pits.elevations_m[18] = 1.0f;
    AnalysisOptions candidate_limited = constrained;
    candidate_limited.maximum_breach_candidates = 1;
    bool budget_exceeded = false;
    try { (void)analyze(multiple_pits, candidate_limited); }
    catch (const std::runtime_error &) { budget_exceeded = true; }
    assert(budget_exceeded);

    // A real valley with one flat intermediate step must route over the flat
    // into lower cells, even without filling any depression.
    ElevationGrid stepped = grid;
    stepped.elevations_m = {
        9, 9, 9, 9, 9,
        8, 8, 8, 8, 8,
        7, 7, 7, 7, 7,
        6, 6, 6, 6, 6,
        5, 5, 5, 5, 5
    };
    const AnalysisResult steps = analyze(stepped, no_fill);
    assert(steps.downstream[12] >= 0);
    assert(steps.potential_fill_cell_count == 0);

    // A larger pseudo-random landscape is a useful regression for cyclic
    // flow and accumulation consistency across interior grid boundaries.
    ElevationGrid region = grid;
    region.columns = 240;
    region.rows = 180;
    region.geometry.pixel_width = 0.0003;
    region.geometry.pixel_height = 0.0003;
    region.elevations_m.resize(std::size_t(region.columns) * region.rows);
    for (std::uint32_t row = 0; row < region.rows; ++row)
    {
        for (std::uint32_t col = 0; col < region.columns; ++col)
        {
            const int noise = int((row * 7919U + col * 104729U + row * col * 13U) % 31U);
            region.elevations_m[std::size_t(row) * region.columns + col] =
                float(1000 - int(row) / 2 - int(col) / 3 + noise);
        }
    }
    AnalysisResult region_result = analyze(region);
    std::uint64_t total_outlet_cells = 0;
    for (std::uint64_t index = 0; index < region_result.downstream.size(); ++index)
    {
        const std::int64_t next = region_result.downstream[std::size_t(index)];
        if (next < 0)
            total_outlet_cells += region_result.upstream_cell_count[std::size_t(index)];
        else
            assert(region_result.conditioned_elevations_m[std::size_t(index)] >=
                   region_result.conditioned_elevations_m[std::size_t(next)]);
    }
    assert(total_outlet_cells == region_result.downstream.size());
    // Pixel-centre coordinates must be preserved independently of job patches.
    double longitude = 0.0;
    double latitude = 0.0;
    cellCoordinate(region, 0, &longitude, &latitude);
    assert(std::abs(longitude - (region.geometry.origin_x + 0.5 * region.geometry.pixel_width)) < 1e-12);
    assert(std::abs(latitude - (region.geometry.origin_y - 0.5 * region.geometry.pixel_height)) < 1e-12);

    // Cancellation is checked inside the hydrology algorithm, not only
    // between fetch/analyze/save phases of the surrounding job service.
    std::atomic<bool> cancellation {true};
    bool interrupted = false;
    try { (void)analyze(region, AnalysisOptions(), &cancellation); }
    catch (const std::runtime_error &) { interrupted = true; }
    assert(interrupted);

    std::cout << "Hydrology core tests passed\n";
}
