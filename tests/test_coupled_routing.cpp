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
bool near(double first, double second, double relative = 1e-9)
{
    return std::abs(first - second) <= relative *
        std::max({1.0, std::abs(first), std::abs(second)});
}

void invalid(const std::function<void()> &call)
{
    bool caught = false;
    try { call(); }
    catch (const std::invalid_argument &) { caught = true; }
    assert(caught);
}

SpillReservoirCurve storageCurve()
{
    return {{{0.0, 0.0}, {10.0, 1000.0}, {20.0, 2000.0}}};
}

BasinSpillJunction junction(std::uint64_t first, std::uint64_t second)
{
    BasinSpillJunction value;
    value.saddle_elevation_m = 10.0;
    BasinSpillContact left;
    left.child_basin_index = first;
    left.cardinal_contact_faces = 1;
    left.cardinal_boundary_length_m = 5.0;
    BasinSpillContact right = left;
    right.child_basin_index = second;
    value.child_contacts = {left, right};
    return value;
}

CoupledConnection weir(std::uint64_t first, std::uint64_t second)
{
    CoupledConnection result;
    result.junction = junction(first, second);
    result.weir.first_component_index = first;
    result.weir.second_component_index = second;
    result.weir.crest_elevation_m = 10.0;
    result.weir.effective_width_m = 2.0;
    result.weir.discharge_factor = 0.9;
    result.submergence.points = {{0.0, 1.0}, {0.5, 0.8}, {1.0, 0.0}};
    return result;
}

CoupledConnection orifice(std::uint64_t first, std::uint64_t second)
{
    CoupledConnection result;
    result.type = CoupledControlType::FullySubmergedRectangularOrifice;
    result.junction = junction(first, second);
    result.orifice.first_component_index = first;
    result.orifice.second_component_index = second;
    result.orifice.invert_elevation_m = 7.0;
    result.orifice.opening_height_m = 2.0;
    result.orifice.opening_width_m = 1.0;
    result.orifice.discharge_factor = 0.7;
    return result;
}

CoupledReservoir reservoir(std::uint64_t id, double initial)
{
    CoupledReservoir result;
    result.component_index = id;
    result.curve = storageCurve();
    result.initial_storage_m3 = initial;
    return result;
}
}

int main()
{
    const std::vector<CoupledReservoir> pair = {reservoir(10, 1200.0), reservoir(20, 800.0)};
    const std::vector<CoupledConnection> link = {weir(10, 20)};
    CoupledRoutingOptions options;
    options.maximum_substep_seconds = 0.01;
    const CoupledRoutingResult first = routeCoupledReservoirs(pair, link,
        {{0.1, {0.0, 0.0}}}, options);
    assert(first.snapshots.size() == 2);
    assert(first.snapshots[0].storage_m3[0] == 1200.0);
    const double initial_rate = rateCalibratedBroadCrestedSpill(
        link[0].junction, link[0].weir, link[0].submergence, 12.0, 8.0).signed_discharge_m3_s;
    assert(first.snapshots.back().storage_m3[0] < 1200.0);
    assert(first.snapshots.back().storage_m3[1] > 800.0);
    assert(near(first.snapshots.back().connection_transfer_m3[0], initial_rate * 0.1, 2e-2));
    assert(near(first.snapshots.back().total_storage_m3, 2000.0));
    assert(near(first.snapshots.back().mass_balance_error_m3, 0.0));
    assert(first.accepted_substeps >= 10);

    // Inflows are externally supplied volumes over each interval, not assumed
    // to arrive at its beginning or instantaneously reach the receiving pond.
    const CoupledRoutingResult with_inflow = routeCoupledReservoirs(
        pair, link, {{1.0, {100.0, 50.0}}, {2.0, {40.0, 10.0}}}, options);
    assert(with_inflow.snapshots.size() == 3);
    assert(near(with_inflow.snapshots[1].total_storage_m3, 2150.0));
    assert(near(with_inflow.snapshots[2].total_storage_m3, 2200.0));
    assert(near(with_inflow.snapshots[2].cumulative_inflow_m3, 200.0));
    assert(near(with_inflow.snapshots[2].elapsed_seconds, 3.0));

    // First-order convergence: refining the maximum hydraulic step should
    // decrease deviation from a much finer reference trajectory.
    CoupledRoutingOptions medium;
    medium.maximum_substep_seconds = 0.25;
    CoupledRoutingOptions fine;
    fine.maximum_substep_seconds = 0.01;
    const CoupledRoutingResult medium_run = routeCoupledReservoirs(
        {reservoir(10, 1400.0), reservoir(20, 1100.0)}, link,
        {{10.0, {0.0, 0.0}}}, medium);
    const CoupledRoutingResult fine_run = routeCoupledReservoirs(
        {reservoir(10, 1400.0), reservoir(20, 1100.0)}, link,
        {{10.0, {0.0, 0.0}}}, fine);
    assert(std::abs(medium_run.snapshots.back().storage_m3[0] -
                    fine_run.snapshots.back().storage_m3[0]) < 1.0);

    // Two simultaneous connections draw from the same upstream reservoir.
    // Reversing their ordering MUST NOT affect the reservoir states.
    const std::vector<CoupledReservoir> triple = {
        reservoir(10, 1700.0), reservoir(20, 1050.0), reservoir(30, 1050.0)};
    const CoupledConnection left = weir(10, 20);
    const CoupledConnection right = weir(10, 30);
    CoupledRoutingOptions coarse;
    coarse.maximum_substep_seconds = 100.0;
    coarse.maximum_substeps = 200000;
    const CoupledRoutingResult normal = routeCoupledReservoirs(
        triple, {left, right}, {{200.0, {0.0, 0.0, 0.0}}}, coarse);
    const CoupledRoutingResult reversed = routeCoupledReservoirs(
        triple, {right, left}, {{200.0, {0.0, 0.0, 0.0}}}, coarse);
    // The coarse step is detected either as a rejected substep or as an
    // equalization-limited transfer; both remain order independent.
    assert(normal.rejected_substeps + normal.equalization_limited_transfers > 0);
    assert(normal.equalization_limited_transfers == reversed.equalization_limited_transfers);
    for (std::size_t i = 0; i < triple.size(); ++i)
        assert(near(normal.snapshots.back().storage_m3[i], reversed.snapshots.back().storage_m3[i]));
    assert(near(normal.snapshots.back().total_storage_m3, 3800.0));
    assert(normal.snapshots.back().storage_m3[0] >= 1000.0);
    assert(normal.snapshots.back().storage_m3[1] >= 1050.0);
    assert(normal.snapshots.back().storage_m3[2] >= 1050.0);

    // Fully submerged opening and explicit refusal of partial opening.
    const CoupledRoutingResult opening = routeCoupledReservoirs(
        {reservoir(10, 1300.0), reservoir(20, 1100.0)}, {orifice(10, 20)},
        {{1.0, {0.0, 0.0}}}, options);
    assert(opening.snapshots.back().connection_transfer_m3[0] > 0.0);
    assert(near(opening.snapshots.back().total_storage_m3, 2400.0));
    invalid([&] {
        routeCoupledReservoirs(pair, {orifice(10, 20)},
                               {{0.1, {0.0, 0.0}}}, options);
    });

    // Invalid topology, curves, event data, and explicit work limits.
    invalid([&] {
        routeCoupledReservoirs({reservoir(10, 1500), reservoir(10, 300)}, {}, {}, options);
    });
    invalid([&] {
        routeCoupledReservoirs(pair, {weir(10, 999)}, {}, options);
    });
    invalid([&] {
        routeCoupledReservoirs(pair, link, {{1.0, {-1.0, 0.0}}}, options);
    });
    invalid([&] {
        routeCoupledReservoirs(pair, link, {{0.0, {0.0, 0.0}}}, options);
    });
    invalid([&] {
        CoupledRoutingOptions exhausted;
        exhausted.maximum_substep_seconds = 0.01;
        exhausted.maximum_substeps = 1;
        routeCoupledReservoirs(pair, link, {{1.0, {0.0, 0.0}}}, exhausted);
    });
    invalid([&] {
        CoupledReservoir invalid_curve = reservoir(10, 500.0);
        invalid_curve.curve.points[1].stored_volume_m3 = 0.0;
        routeCoupledReservoirs({invalid_curve}, {}, {}, options);
    });
    invalid([&] {
        const double huge = std::numeric_limits<double>::max();
        CoupledRoutingOptions huge_step;
        huge_step.maximum_substep_seconds = huge;
        routeCoupledReservoirs({reservoir(10, 0.0)}, {},
                               {{huge, {0.0}}, {huge, {0.0}}}, huge_step);
    });
    std::atomic<bool> canceled(true);
    bool was_canceled = false;
    try { routeCoupledReservoirs(pair, link, {}, options, &canceled); }
    catch (const std::runtime_error &) { was_canceled = true; }
    assert(was_canceled);

    // A valid no-flow event must not invent transfers or hydraulic timing.
    const CoupledRoutingResult dry = routeCoupledReservoirs(
        {reservoir(10, 500.0), reservoir(20, 500.0)}, link,
        {{5.0, {0.0, 0.0}}}, options);
    assert(dry.snapshots.back().connection_transfer_m3[0] == 0.0);
    assert(near(dry.snapshots.back().total_storage_m3, 1000.0));

    // More independent, analytically bounded inflows: transfer may be
    // distributed differently, but initial+inflow must always equal stored.
    for (int seed = 1; seed <= 35; ++seed)
    {
        const double left_initial = 1000.0 + seed * 8.0;
        const double right_initial = 1010.0 + seed * 4.0;
        const double left_inflow = 10.0 + (seed % 9);
        const double right_inflow = 5.0 + (seed % 5);
        const CoupledRoutingResult trial = routeCoupledReservoirs(
            {reservoir(10, left_initial), reservoir(20, right_initial)}, link,
            {{4.0, {left_inflow, right_inflow}}}, medium);
        const CoupledRoutingSnapshot &end = trial.snapshots.back();
        assert(near(end.total_storage_m3,
                    left_initial + right_initial + left_inflow + right_inflow));
        assert(std::abs(end.mass_balance_error_m3) < 1e-7);
        assert(end.storage_m3[0] >= 0.0 && end.storage_m3[1] >= 0.0);
    }

    // No connections, still integrates only the specified inflow volumes.
    const CoupledRoutingResult standalone = routeCoupledReservoirs(
        {reservoir(42, 100.0)}, {}, {{1.0, {12.0}}, {2.0, {25.0}}}, options);
    assert(near(standalone.snapshots.back().storage_m3[0], 137.0));
    assert(near(standalone.snapshots.back().cumulative_inflow_m3, 37.0));

    // Regression: an orifice (Q ~ sqrt(head)) previously exhausted the
    // substep budget as soon as both stages approached equality (about 21 s
    // for this pair), because explicit substeps had to shrink with
    // sqrt(head). Long runs must now complete with full substeps and settle
    // at the common level without crossing it.
    {
        const CoupledRoutingOptions defaults;
        for (const double duration : {60.0, 3600.0, 86400.0})
        {
            const CoupledRoutingResult settled = routeCoupledReservoirs(
                {reservoir(10, 1300.0), reservoir(20, 1100.0)}, {orifice(10, 20)},
                {{duration, {0.0, 0.0}}}, defaults);
            const CoupledRoutingSnapshot &last = settled.snapshots.back();
            assert(settled.accepted_substeps <= std::uint64_t(duration) + 1);
            assert(settled.equalization_limited_transfers > 0);
            assert(near(last.water_level_m[0], last.water_level_m[1], 1e-9));
            assert(near(last.storage_m3[0], 1200.0, 1e-9));
            assert(near(last.total_storage_m3, 2400.0));
            assert(std::abs(last.mass_balance_error_m3) < 1e-7);
        }

        // Before equalization the limiter is inactive and the scheme still
        // converges to the analytic solution. Equal 100 m2 reservoirs give
        // d(head)/dt = -2 C sqrt(head) / A, so sqrt(head) falls linearly.
        const double c = rateFullySubmergedOrifice(orifice(10, 20).junction,
            orifice(10, 20).orifice, 12.0, 11.0).signed_discharge_m3_s;
        const double analytic_head = std::pow(std::sqrt(2.0) - c * 10.0 / 100.0, 2.0);
        CoupledRoutingOptions fine_steps;
        fine_steps.maximum_substep_seconds = 0.001;
        fine_steps.maximum_substeps = 100000;
        const CoupledRoutingResult early = routeCoupledReservoirs(
            {reservoir(10, 1300.0), reservoir(20, 1100.0)}, {orifice(10, 20)},
            {{10.0, {0.0, 0.0}}}, fine_steps);
        const double head = early.snapshots.back().water_level_m[0] -
                            early.snapshots.back().water_level_m[1];
        assert(early.equalization_limited_transfers == 0);
        assert(std::abs(head - analytic_head) < 2e-3);

        // Equalized reservoirs with unequal inflow: the receiver may rise
        // above the donor because of its own inflow. That is physical and
        // must not trigger an endless rejection cascade (weir and orifice).
        for (const CoupledConnection &control : {orifice(10, 20), weir(10, 20)})
        {
            const double start = control.type == CoupledControlType::CalibratedBroadCrestedWeir
                ? 1100.0 : 1200.0;
            const CoupledRoutingResult wet = routeCoupledReservoirs(
                {reservoir(10, start), reservoir(20, start)}, {control},
                {{3600.0, {0.0, 400.0}}, {3600.0, {0.0, 0.0}}}, defaults);
            const CoupledRoutingSnapshot &end = wet.snapshots.back();
            assert(wet.accepted_substeps <= 7201);
            assert(near(end.total_storage_m3, 2.0 * start + 400.0));
            assert(std::abs(end.mass_balance_error_m3) < 1e-7);
            assert(end.connection_transfer_m3[0] < 0.0); // flows back towards 10
        }
    }
}
