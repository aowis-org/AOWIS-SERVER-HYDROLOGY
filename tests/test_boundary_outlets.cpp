#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/coupled_routing.h>

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
bool near(double first, double second, double tolerance = 1e-8)
{
    return std::abs(first - second) <= tolerance *
        std::max({1.0, std::abs(first), std::abs(second)});
}

CoupledReservoir reservoir(std::uint64_t id, double initial)
{
    CoupledReservoir result;
    result.component_index = id;
    result.curve.points = {{0.0, 0.0}, {10.0, 1000.0}, {20.0, 2000.0}};
    result.initial_storage_m3 = initial;
    return result;
}

CoupledBoundaryOutlet outlet(std::uint64_t id, double width = 2.0)
{
    CoupledBoundaryOutlet result;
    result.reservoir_component_index = id;
    result.crest_elevation_m = 10.0;
    result.effective_width_m = width;
    result.discharge_factor = 0.9;
    result.downstream_water_level_m = 8.0;
    return result;
}

void invalid(const std::function<void()> &function)
{
    bool thrown = false;
    try { function(); }
    catch (const std::invalid_argument &) { thrown = true; }
    assert(thrown);
}
}

int main()
{
    CoupledRoutingOptions options;
    options.maximum_substep_seconds = 0.01;
    const CoupledBoundaryOutlet exit = outlet(10);
    const std::vector<CoupledReservoir> basins = {reservoir(10, 1200.0)};
    const CoupledRoutingResult short_event = routeCoupledReservoirsWithOutlets(
        basins, {}, {exit}, {{0.1, {0.0}}}, options);
    const CoupledRoutingSnapshot &one = short_event.snapshots.back();
    assert(short_event.snapshots.size() == 2);
    assert(one.boundary_outflow_m3.size() == 1);
    const double critical_depth = 2.0 * 2.0 / 3.0;
    const double discharge = 0.9 * 2.0 * std::sqrt(9.80665) *
        critical_depth * std::sqrt(critical_depth);
    assert(near(one.boundary_outflow_m3[0], discharge * 0.1, 0.005));
    assert(near(one.cumulative_boundary_outflow_m3, one.boundary_outflow_m3[0]));
    assert(near(one.storage_m3[0] + one.cumulative_boundary_outflow_m3, 1200.0));
    assert(near(one.mass_balance_error_m3, 0.0));
    assert(short_event.snapshots.front().boundary_outflow_m3[0] == 0.0);

    // A long period of free drainage must never lower the pond below the
    // control crest. The outflow is time integrated and conserves volume.
    CoupledRoutingOptions long_options;
    long_options.maximum_substep_seconds = 100.0;
    const CoupledRoutingResult drained = routeCoupledReservoirsWithOutlets(
        basins, {}, {exit}, {{10000.0, {0.0}}}, long_options);
    assert(drained.snapshots.back().storage_m3[0] >= 999.999999);
    assert(drained.snapshots.back().storage_m3[0] < 1001.0);
    assert(near(drained.snapshots.back().total_storage_m3 +
                drained.snapshots.back().cumulative_boundary_outflow_m3, 1200.0));
    assert(drained.snapshots.back().boundary_outflow_m3[0] >= 199.0);
    assert(drained.rejected_substeps > 0);

    // Dry outlet never produces a negative flow or artificial withdrawal.
    const CoupledRoutingResult dry = routeCoupledReservoirsWithOutlets(
        {reservoir(10, 800.0)}, {}, {exit}, {{10.0, {0.0}}}, options);
    assert(dry.snapshots.back().boundary_outflow_m3[0] == 0.0);
    assert(near(dry.snapshots.back().total_storage_m3, 800.0));

    // Boundary outflow, initial storage, and explicitly prescribed inflows
    // must balance for each interval, not just at the final time.
    const CoupledRoutingResult rain = routeCoupledReservoirsWithOutlets(
        {reservoir(10, 1100.0)}, {}, {exit},
        {{5.0, {100.0}}, {5.0, {50.0}}, {5.0, {0.0}}}, options);
    for (const CoupledRoutingSnapshot &sample : rain.snapshots)
    {
        assert(near(sample.total_storage_m3 + sample.cumulative_boundary_outflow_m3,
                    1100.0 + sample.cumulative_inflow_m3));
        assert(near(sample.mass_balance_error_m3, 0.0));
    }
    assert(rain.snapshots.back().boundary_outflow_m3[0] > 0.0);

    // Simultaneous controls on one basin are order-independent. Cumulative
    // per-outlet volumes retain caller ordering, not drainage priority.
    const CoupledBoundaryOutlet narrow = outlet(10, 1.0);
    const CoupledBoundaryOutlet wide = outlet(10, 3.0);
    const CoupledRoutingResult forwards = routeCoupledReservoirsWithOutlets(
        {reservoir(10, 1500.0)}, {}, {narrow, wide},
        {{15.0, {0.0}}}, options);
    const CoupledRoutingResult backwards = routeCoupledReservoirsWithOutlets(
        {reservoir(10, 1500.0)}, {}, {wide, narrow},
        {{15.0, {0.0}}}, options);
    const CoupledRoutingSnapshot &forward = forwards.snapshots.back();
    const CoupledRoutingSnapshot &backward = backwards.snapshots.back();
    assert(near(forward.total_storage_m3, backward.total_storage_m3));
    assert(near(forward.cumulative_boundary_outflow_m3,
                backward.cumulative_boundary_outflow_m3));
    assert(near(forward.boundary_outflow_m3[0], backward.boundary_outflow_m3[1]));
    assert(near(forward.boundary_outflow_m3[1], backward.boundary_outflow_m3[0]));
    assert(near(forward.total_storage_m3 + forward.cumulative_boundary_outflow_m3, 1500.0));

    // Closed-network method remains source-compatible and never reports
    // exterior discharge. The open variant with zero outlets agrees exactly.
    const CoupledRoutingResult legacy = routeCoupledReservoirs(
        basins, {}, {{1.0, {25.0}}}, options);
    const CoupledRoutingResult empty_outlets = routeCoupledReservoirsWithOutlets(
        basins, {}, {}, {{1.0, {25.0}}}, options);
    assert(legacy.snapshots.back().boundary_outflow_m3.empty());
    assert(legacy.snapshots.back().cumulative_boundary_outflow_m3 == 0.0);
    assert(near(legacy.snapshots.back().total_storage_m3,
                empty_outlets.snapshots.back().total_storage_m3));

    // An internal weir feeding an exterior outlet must combine its transfer
    // and boundary discharge in a single simultaneous mass balance.
    CoupledConnection internal;
    internal.junction.saddle_elevation_m = 10.0;
    for (const std::uint64_t id : {10ULL, 20ULL})
    {
        BasinSpillContact contact;
        contact.child_basin_index = id;
        contact.cardinal_contact_faces = 1;
        contact.cardinal_boundary_length_m = 4.0;
        internal.junction.child_contacts.push_back(contact);
    }
    internal.weir.first_component_index = 10;
    internal.weir.second_component_index = 20;
    internal.weir.crest_elevation_m = 10.0;
    internal.weir.effective_width_m = 2.0;
    internal.weir.discharge_factor = 0.9;
    internal.submergence.points = {{0.0, 1.0}, {0.5, 0.75}, {1.0, 0.0}};
    const CoupledRoutingResult linked = routeCoupledReservoirsWithOutlets(
        {reservoir(10, 1400.0), reservoir(20, 1200.0)}, {internal}, {outlet(20)},
        {{4.0, {100.0, 40.0}}}, options);
    const CoupledRoutingSnapshot &linked_end = linked.snapshots.back();
    assert(linked_end.connection_transfer_m3.size() == 1);
    assert(linked_end.connection_transfer_m3[0] > 0.0);
    assert(linked_end.boundary_outflow_m3.size() == 1);
    assert(linked_end.boundary_outflow_m3[0] > 0.0);
    assert(near(linked_end.total_storage_m3 + linked_end.cumulative_boundary_outflow_m3,
                2600.0 + 140.0));
    assert(near(linked_end.mass_balance_error_m3, 0.0));

    // Diverse independent rainfall events: the exterior never injects water
    // and volume is conserved across several supplied runoff intervals.
    for (int seed = 1; seed <= 25; ++seed)
    {
        const double initial = 1050.0 + seed * 12.0;
        const CoupledRoutingResult trial = routeCoupledReservoirsWithOutlets(
            {reservoir(10, initial)}, {}, {outlet(10, 0.5 + seed * 0.1)},
            {{2.0, {static_cast<double>(seed)}}, {3.0, {0.0}}}, options);
        const CoupledRoutingSnapshot &final_state = trial.snapshots.back();
        assert(near(final_state.total_storage_m3 + final_state.cumulative_boundary_outflow_m3,
                    initial + seed));
        assert(final_state.storage_m3[0] >= 1000.0 - 1e-8);
        assert(final_state.cumulative_boundary_outflow_m3 >= 0.0);
    }

    invalid([&] { routeCoupledReservoirsWithOutlets(basins, {}, {outlet(999)}, {}, options); });
    invalid([&] {
        CoupledBoundaryOutlet bad = exit;
        bad.effective_width_m = 0.0;
        routeCoupledReservoirsWithOutlets(basins, {}, {bad}, {}, options);
    });
    invalid([&] {
        CoupledBoundaryOutlet bad = exit;
        bad.discharge_factor = 1.1;
        routeCoupledReservoirsWithOutlets(basins, {}, {bad}, {}, options);
    });
    invalid([&] {
        CoupledBoundaryOutlet bad = exit;
        bad.downstream_water_level_m = bad.crest_elevation_m;
        routeCoupledReservoirsWithOutlets(basins, {}, {bad}, {}, options);
    });
    invalid([&] {
        CoupledBoundaryOutlet bad = exit;
        bad.crest_elevation_m = 25.0;
        routeCoupledReservoirsWithOutlets(basins, {}, {bad}, {}, options);
    });
    invalid([&] {
        CoupledBoundaryOutlet bad = exit;
        bad.effective_width_m = std::numeric_limits<double>::quiet_NaN();
        routeCoupledReservoirsWithOutlets(basins, {}, {bad}, {}, options);
    });
    std::atomic<bool> cancelled(true);
    bool threw = false;
    try { routeCoupledReservoirsWithOutlets(basins, {}, {exit}, {}, options, &cancelled); }
    catch (const std::runtime_error &) { threw = true; }
    assert(threw);
}
