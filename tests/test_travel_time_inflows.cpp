#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/travel_time_inflows.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace Aowis::Hydrology;

namespace
{
ElevationGrid utmGrid(std::uint32_t columns, std::uint32_t rows,
                      const std::vector<float> &elevations)
{
    ElevationGrid dem;
    dem.columns = columns;
    dem.rows = rows;
    dem.geometry.horizontal_crs = HorizontalCrs::Wgs84Utm;
    dem.geometry.utm_zone = 32;
    dem.geometry.origin_x = 500000.0;
    dem.geometry.origin_y = 5500000.0;
    dem.geometry.pixel_width = 10.0;
    dem.geometry.pixel_height = 10.0;
    dem.elevations_m = elevations;
    return dem;
}

CoupledReservoir reservoir(std::uint64_t index)
{
    CoupledReservoir result;
    result.component_index = index;
    result.curve.points = {{0.0, 0.0}, {100.0, 1.0e7}};
    return result;
}

bool near(double lhs, double rhs)
{
    return std::abs(lhs - rhs) <= 1e-8 * std::max({1.0, std::abs(lhs), std::abs(rhs)});
}

void invalid(const std::function<void()> &callback)
{
    bool thrown = false;
    try { callback(); }
    catch (const std::invalid_argument &) { thrown = true; }
    assert(thrown);
}
}

int main()
{
    const ElevationGrid dem = utmGrid(5, 5, {
         5,20,20,20,20,
        20,15,15,15,20,
        20,15,10,15,20,
        20,15,15,15,20,
        20,20,20,20,20
    });
    const DepressionHierarchy hierarchy = buildDepressionHierarchy(dem);
    assert(hierarchy.basins.size() == 1);
    const std::vector<CoupledReservoir> reservoirs = {reservoir(0)};
    std::vector<double> runoff(25, 0.0);
    runoff[12] = 100.0; // Basin minimum: zero travel delay.
    runoff[7] = 80.0;  // Adjacent cell: one UTM pixel of travel.
    runoff[0] = 30.0;  // Boundary minimum: direct exit, no basin inflow.
    TravelTimeOptions options;
    options.effective_velocity_m_per_s = 10.0;
    options.output_interval_seconds = 1.0;
    const TravelTimeRunoffForcing delayed = travelTimeInflowsFromRaster(
        dem, hierarchy, runoff, reservoirs, {{2.0, 1.0}}, 2.0, options);
    assert(near(delayed.forcing.generated_runoff_m3, 210.0));
    assert(near(delayed.forcing.delivered_to_basins_m3, 180.0));
    assert(near(delayed.forcing.direct_boundary_outflow_m3, 30.0));
    assert(delayed.maximum_source_travel_seconds > 0.999 &&
           delayed.maximum_source_travel_seconds < 1.002);
    assert(delayed.forcing.basin_inflows.size() == 4 ||
           delayed.forcing.basin_inflows.size() == 3);
    const std::vector<CoupledInflowInterval> &bins = delayed.forcing.basin_inflows;
    assert(near(bins[0].inflow_volume_m3[0], 50.0));
    assert(near(bins[1].inflow_volume_m3[0],
                50.0 + 40.0 * (2.0 - delayed.maximum_source_travel_seconds)));
    assert(bins[2].inflow_volume_m3[0] > 39.9);
    assert(near(delayed.forcing.direct_boundary_outflow_m3_by_interval[0], 15.0));
    assert(near(delayed.forcing.direct_boundary_outflow_m3_by_interval[1], 15.0));
    double total_basin = 0.0;
    double total_boundary = 0.0;
    for (std::size_t t = 0; t < bins.size(); ++t)
    {
        total_basin += bins[t].inflow_volume_m3[0];
        total_boundary += delayed.forcing.direct_boundary_outflow_m3_by_interval[t];
        assert(bins[t].duration_seconds > 0.0);
    }
    assert(near(total_basin, 180.0));
    assert(near(total_boundary, 30.0));
    assert(near(delayed.forcing.mass_balance_error_m3, 0.0));
    const CoupledRoutingResult routed = routeCoupledReservoirs(
        reservoirs, {}, bins);
    assert(near(routed.snapshots.back().total_storage_m3, 180.0));
    assert(near(routed.snapshots.back().elapsed_seconds, delayed.output_duration_seconds));

    // At twice the velocity the lag halves; at one-half velocity the same
    // volume appears later, without changing any spatial destination.
    options.effective_velocity_m_per_s = 20.0;
    const TravelTimeRunoffForcing faster = travelTimeInflowsFromRaster(
        dem, hierarchy, runoff, reservoirs, {{2.0, 1.0}}, 2.0, options);
    assert(near(faster.maximum_source_travel_seconds * 2.0,
                delayed.maximum_source_travel_seconds));
    assert(near(faster.forcing.basin_inflows[0].inflow_volume_m3[0],
                50.0 + 40.0 * (1.0 - faster.maximum_source_travel_seconds)));
    options.effective_velocity_m_per_s = 5.0;
    const TravelTimeRunoffForcing slower = travelTimeInflowsFromRaster(
        dem, hierarchy, runoff, reservoirs, {{2.0, 1.0}}, 2.0, options);
    assert(near(slower.maximum_source_travel_seconds,
                delayed.maximum_source_travel_seconds * 2.0));
    assert(near(slower.forcing.basin_inflows[0].inflow_volume_m3[0], 50.0));

    // A dry D8 route to the domain boundary can also have positive delay.
    // A source at (1,1) descends diagonally to the low corner at (0,0).
    std::vector<double> edge_runoff(25, 0.0);
    edge_runoff[6] = 20.0;
    options.effective_velocity_m_per_s = 10.0;
    const TravelTimeRunoffForcing boundary_delayed = travelTimeInflowsFromRaster(
        dem, hierarchy, edge_runoff, reservoirs, {{2.0, 1.0}}, 2.0, options);
    assert(boundary_delayed.maximum_source_travel_seconds > 1.4);
    assert(near(boundary_delayed.forcing.direct_boundary_outflow_m3, 20.0));
    assert(near(boundary_delayed.forcing.direct_boundary_outflow_m3_by_interval[0], 0.0));
    assert(boundary_delayed.forcing.direct_boundary_outflow_m3_by_interval[1] > 0.0);

    // Direct routing with no basins is valid; no coupled reservoir run is
    // necessary. Zero runoff still has a valid storm-duration horizon.
    const ElevationGrid hillside = utmGrid(3, 3, {
        10,10,10,10,12,10,10,10,10
    });
    const DepressionHierarchy empty = buildDepressionHierarchy(hillside);
    assert(empty.basins.empty());
    const std::vector<double> no_runoff(9, 0.0);
    const TravelTimeRunoffForcing dry = travelTimeInflowsFromRaster(
        hillside, empty, no_runoff, {}, {{2.0, 0.4}, {3.0, 0.6}}, 5.0, options);
    assert(near(dry.forcing.generated_runoff_m3, 0.0));
    assert(near(dry.output_duration_seconds, 5.0));
    assert(dry.forcing.basin_inflows.size() == 5);

    // Different profile intervals and output bins, with a delayed tail.
    const TravelTimeRunoffForcing mixed = travelTimeInflowsFromRaster(
        dem, hierarchy, runoff, reservoirs,
        {{0.5, 0.25}, {1.5, 0.75}}, 2.0, options);
    assert(near(mixed.forcing.generated_runoff_m3, 210.0));
    assert(near(mixed.forcing.mass_balance_error_m3, 0.0));

    // Two basin leaves, supplied in reverse order. The adapter must place
    // runoff into the caller's reservoir columns, not sort by basin index.
    const ElevationGrid pair = utmGrid(7, 5, {
        16,16,16,16,16,16,16,
        16,16,16,16,16,16,16,
        16,10,12,12,12,10,16,
        16,16,16,16,16,16,16,
        16,16,16,16,16,16,16
    });
    const DepressionHierarchy paired = buildDepressionHierarchy(pair);
    assert(paired.basins.size() == 3);
    std::vector<double> pair_runoff(35, 0.0);
    pair_runoff[15] = 70.0;
    pair_runoff[19] = 30.0;
    const std::vector<CoupledReservoir> backwards = {reservoir(1), reservoir(0)};
    const TravelTimeRunoffForcing pair_forcing = travelTimeInflowsFromRaster(
        pair, paired, pair_runoff, backwards, {{4.0, 0.6}, {6.0, 0.4}}, 10.0, options);
    double left = 0.0;
    double right = 0.0;
    for (const CoupledInflowInterval &interval : pair_forcing.forcing.basin_inflows)
    {
        left += interval.inflow_volume_m3[1];
        right += interval.inflow_volume_m3[0];
    }
    assert(near(left, 70.0));
    assert(near(right, 30.0));
    assert(near(pair_forcing.forcing.mass_balance_error_m3, 0.0));

    // Long rectangular pulses must be deposited using entire interval
    // overlaps, including many wholly covered bins and the final short bin.
    options.output_interval_seconds = 0.1;
    const TravelTimeRunoffForcing many_bins = travelTimeInflowsFromRaster(
        dem, hierarchy, runoff, reservoirs, {{1.3, 0.15}, {8.7, 0.85}},
        10.0, options);
    assert(many_bins.forcing.basin_inflows.size() >= 110);
    long double bsum = 0.0L;
    long double dsum = 0.0L;
    for (std::size_t t = 0; t < many_bins.forcing.basin_inflows.size(); ++t)
    {
        bsum += many_bins.forcing.basin_inflows[t].inflow_volume_m3[0];
        dsum += many_bins.forcing.direct_boundary_outflow_m3_by_interval[t];
    }
    assert(near(double(bsum), 180.0));
    assert(near(double(dsum), 30.0));
    assert(near(many_bins.forcing.mass_balance_error_m3, 0.0));
    options.output_interval_seconds = 1.0;

    // Geographic WGS84 grids use geodesic (not degree-as-metre) distances.
    ElevationGrid geographic = dem;
    geographic.geometry.horizontal_crs = HorizontalCrs::Wgs84Geographic;
    geographic.geometry.utm_zone = 0;
    geographic.geometry.origin_x = 8.0;
    geographic.geometry.origin_y = 50.0;
    geographic.geometry.pixel_width = 0.0001;
    geographic.geometry.pixel_height = 0.0001;
    const DepressionHierarchy geo_hierarchy = buildDepressionHierarchy(geographic);
    const TravelTimeRunoffForcing geo_forcing = travelTimeInflowsFromRaster(
        geographic, geo_hierarchy, runoff, {reservoir(0)},
        {{2.0, 1.0}}, 2.0, options);
    assert(near(geo_forcing.forcing.generated_runoff_m3, 210.0));
    assert(geo_forcing.maximum_source_travel_seconds > 0.0);
    assert(near(geo_forcing.forcing.mass_balance_error_m3, 0.0));

    // The older adapter permits tiny duration-rounding differences. The
    // last shifted pulse must still be retained, not truncated at the nominal
    // event duration.
    const TravelTimeRunoffForcing rounded = travelTimeInflowsFromRaster(
        dem, hierarchy, runoff, reservoirs,
        {{2.0 + 1e-10, 1.0}}, 2.0, options);
    assert(near(rounded.forcing.generated_runoff_m3, 210.0));
    assert(near(rounded.forcing.mass_balance_error_m3, 0.0));

    options.effective_velocity_m_per_s = 0.0;
    invalid([&] { travelTimeInflowsFromRaster(
        dem, hierarchy, runoff, reservoirs, {{2.0, 1.0}}, 2.0, options); });
    options.effective_velocity_m_per_s = 1.0;
    options.output_interval_seconds = -1.0;
    invalid([&] { travelTimeInflowsFromRaster(
        dem, hierarchy, runoff, reservoirs, {{2.0, 1.0}}, 2.0, options); });
    options.output_interval_seconds = 1.0;
    options.maximum_output_intervals = 1;
    invalid([&] { travelTimeInflowsFromRaster(
        dem, hierarchy, runoff, reservoirs, {{2.0, 1.0}}, 2.0, options); });
    options.maximum_output_intervals = 10000;
    options.maximum_raster_cells = 10;
    invalid([&] { travelTimeInflowsFromRaster(
        dem, hierarchy, runoff, reservoirs, {{2.0, 1.0}}, 2.0, options); });
    options.maximum_raster_cells = 10000000;
    invalid([&] { travelTimeInflowsFromRaster(
        dem, hierarchy, runoff, reservoirs, {{2.0, 0.7}}, 2.0, options); });
    invalid([&] { travelTimeInflowsFromRaster(
        dem, hierarchy, runoff, {}, {{2.0, 1.0}}, 2.0, options); });
    std::vector<double> negative = runoff;
    negative[7] = -1.0;
    invalid([&] { travelTimeInflowsFromRaster(
        dem, hierarchy, negative, reservoirs, {{2.0, 1.0}}, 2.0, options); });
    std::atomic<bool> cancelled(true);
    bool was_cancelled = false;
    try { travelTimeInflowsFromRaster(
        dem, hierarchy, runoff, reservoirs, {{2.0, 1.0}}, 2.0, options, &cancelled); }
    catch (const std::runtime_error &) { was_cancelled = true; }
    assert(was_cancelled);
    return 0;
}
