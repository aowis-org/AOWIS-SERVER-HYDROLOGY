#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/temporal_inflows.h>

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
ElevationGrid grid(std::uint32_t columns, std::uint32_t rows,
                   const std::vector<float> &elevations)
{
    ElevationGrid result;
    result.columns = columns;
    result.rows = rows;
    result.geometry.origin_x = 8.0;
    result.geometry.origin_y = 50.0;
    result.geometry.pixel_width = 0.001;
    result.geometry.pixel_height = 0.001;
    result.elevations_m = elevations;
    return result;
}

CoupledReservoir reservoir(std::uint64_t basin_index)
{
    CoupledReservoir result;
    result.component_index = basin_index;
    result.curve.points = {{0.0, 0.0}, {100.0, 10000000.0}};
    return result;
}

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-9 * std::max({1.0, std::abs(a), std::abs(b)});
}

void invalid(const std::function<void()> &f)
{
    bool caught = false;
    try { f(); }
    catch (const std::invalid_argument &) { caught = true; }
    assert(caught);
}
}

int main()
{
    // Closed central pit plus directly draining raster-boundary source. The
    // static model can spill after filling, but ONLY direct pit inflow may
    // enter the time-dependent reservoir network.
    const ElevationGrid pit = grid(5, 5, {
         5,20,20,20,20,
        20,15,15,15,20,
        20,15,10,15,20,
        20,15,15,15,20,
        20,20,20,20,20
    });
    const DepressionHierarchy hierarchy = buildDepressionHierarchy(pit);
    assert(!hierarchy.basins.empty());
    std::vector<CoupledReservoir> reservoirs;
    for (const DepressionBasin &basin : hierarchy.basins)
        if (basin.child_indices.empty()) reservoirs.push_back(reservoir(basin.index));
    assert(reservoirs.size() == 1);
    std::vector<double> storm(25, 0.0);
    storm[12] = 150.0;
    storm[0] = 50.0;
    const RunoffRedistribution spatial = redistributeRunoff(pit, hierarchy, storm);
    assert(near(spatial.generated_runoff_m3, 200.0));
    assert(near(spatial.direct_boundary_outflow_m3, 50.0));
    assert(near(spatial.basins[reservoirs[0].component_index].direct_inflow_m3, 150.0));
    const std::vector<TemporalRainfallInterval> profile = {{20.0, 0.25}, {80.0, 0.75}};
    const TemporalRunoffForcing forcing = temporalInflowsFromRaster(
        pit, hierarchy, storm, reservoirs, profile, 100.0);
    assert(forcing.basin_inflows.size() == 2);
    assert(near(forcing.basin_inflows[0].inflow_volume_m3[0], 37.5));
    assert(near(forcing.basin_inflows[1].inflow_volume_m3[0], 112.5));
    assert(near(forcing.direct_boundary_outflow_m3_by_interval[0], 12.5));
    assert(near(forcing.direct_boundary_outflow_m3_by_interval[1], 37.5));
    assert(near(forcing.generated_runoff_m3, 200.0));
    assert(near(forcing.delivered_to_basins_m3, 150.0));
    assert(near(forcing.direct_boundary_outflow_m3, 50.0));
    assert(near(forcing.mass_balance_error_m3, 0.0));
    CoupledRoutingOptions options;
    options.maximum_substep_seconds = 10.0;
    const CoupledRoutingResult solution = routeCoupledReservoirs(
        reservoirs, {}, forcing.basin_inflows, options);
    assert(solution.snapshots.size() == 3);
    assert(near(solution.snapshots.back().total_storage_m3, 150.0));
    assert(near(solution.snapshots.back().cumulative_inflow_m3, 150.0));
    assert(near(solution.snapshots.back().total_storage_m3 +
                forcing.direct_boundary_outflow_m3, 200.0));
    assert(near(solution.snapshots.back().elapsed_seconds, 100.0));

    // A shared parent cannot be counted as an additional reservoir. Reverse
    // reservoir order to confirm leaf indices are mapped into correct columns.
    const ElevationGrid paired = grid(7, 5, {
        16,16,16,16,16,16,16,
        16,16,16,16,16,16,16,
        16,10,12,12,12,10,16,
        16,16,16,16,16,16,16,
        16,16,16,16,16,16,16
    });
    const DepressionHierarchy merged = buildDepressionHierarchy(paired);
    assert(merged.basins.size() == 3);
    std::vector<double> pair_storm(35, 0.0);
    pair_storm[15] = 70.0;
    pair_storm[19] = 30.0;
    const RunoffRedistribution pair_spatial = redistributeRunoff(paired, merged, pair_storm);
    const std::vector<CoupledReservoir> reversed = {reservoir(1), reservoir(0)};
    const TemporalRunoffForcing timed = temporalInflowsFromRedistribution(
        merged, pair_spatial, reversed, {{3.0, 0.0}, {7.0, 1.0}}, 10.0);
    assert(timed.basin_inflows[0].inflow_volume_m3 == std::vector<double>({0.0, 0.0}));
    assert(near(timed.basin_inflows[1].inflow_volume_m3[0], 30.0));
    assert(near(timed.basin_inflows[1].inflow_volume_m3[1], 70.0));
    assert(near(routeCoupledReservoirs(reversed, {}, timed.basin_inflows).snapshots.back().total_storage_m3,
                100.0));

    // A dry raster with no depressions legitimately generates only boundary
    // drainage; the coupled solver itself requires at least one reservoir.
    const ElevationGrid no_pit = grid(3, 3, {10,10,10,10,12,10,10,10,10});
    const DepressionHierarchy no_basins = buildDepressionHierarchy(no_pit);
    assert(no_basins.basins.empty());
    std::vector<double> direct_only(9, 0.0);
    direct_only[4] = 5.0;
    const TemporalRunoffForcing direct = temporalInflowsFromRaster(
        no_pit, no_basins, direct_only, {}, {{60, 1.0}}, 60.0);
    assert(near(direct.direct_boundary_outflow_m3, 5.0));
    assert(direct.basin_inflows[0].inflow_volume_m3.empty());

    invalid([&] { temporalInflowsFromRedistribution(
        merged, pair_spatial, reversed, {{3.0, 0.6}, {7.0, 0.3}}, 10.0); });
    invalid([&] { temporalInflowsFromRedistribution(
        merged, pair_spatial, reversed, {{3.0, 0.5}, {7.0, 0.5}}, 9.0); });
    invalid([&] { temporalInflowsFromRedistribution(
        merged, pair_spatial, reversed, {{0.0, 1.0}}, 10.0); });
    invalid([&] { temporalInflowsFromRedistribution(
        merged, pair_spatial, reversed, {{10.0, -1.0}}, 10.0); });
    invalid([&] { temporalInflowsFromRedistribution(
        merged, pair_spatial, {reservoir(0), reservoir(0)}, {{10, 1.0}}, 10); });
    invalid([&] { temporalInflowsFromRedistribution(
        merged, pair_spatial, {reservoir(0), reservoir(2)}, {{10, 1.0}}, 10); });
    invalid([&] { temporalInflowsFromRedistribution(
        merged, pair_spatial, {reservoir(0)}, {{10, 1.0}}, 10); });
    RunoffRedistribution broken = pair_spatial;
    broken.direct_boundary_outflow_m3 += 20.0;
    invalid([&] { temporalInflowsFromRedistribution(
        merged, broken, reversed, {{10, 1.0}}, 10); });
    broken = pair_spatial;
    broken.basins[2].direct_inflow_m3 += 1.0;
    invalid([&] { temporalInflowsFromRedistribution(
        merged, broken, reversed, {{10, 1.0}}, 10); });
    broken = pair_spatial;
    broken.basins[0].direct_inflow_m3 = std::numeric_limits<double>::infinity();
    invalid([&] { temporalInflowsFromRedistribution(
        merged, broken, reversed, {{10, 1.0}}, 10); });
    std::atomic<bool> cancelled(true);
    bool canceled = false;
    try { temporalInflowsFromRedistribution(
        merged, pair_spatial, reversed, {{10, 1.0}}, 10, &cancelled); }
    catch (const std::runtime_error &) { canceled = true; }
    assert(canceled);
    // One very small time fraction followed by the remainder still sums to
    // the entire original runoff, without independently rounded totals.
    const TemporalRunoffForcing uneven = temporalInflowsFromRedistribution(
        merged, pair_spatial, reversed,
        {{1.0, 1e-12}, {1.0, 1.0 - 1e-12}}, 2.0);
    assert(near(uneven.generated_runoff_m3,
                uneven.delivered_to_basins_m3 + uneven.direct_boundary_outflow_m3));
    return 0;
}
