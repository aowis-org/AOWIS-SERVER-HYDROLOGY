#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/spill_hydraulics.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace Aowis::Hydrology;

namespace
{
ElevationGrid makeGrid(const std::vector<float> &values, std::uint32_t columns,
                       std::uint32_t rows)
{
    ElevationGrid grid;
    grid.columns = columns;
    grid.rows = rows;
    grid.geometry.origin_x = 8.0;
    grid.geometry.origin_y = 50.0;
    grid.geometry.pixel_width = 0.001;
    grid.geometry.pixel_height = 0.001;
    grid.elevations_m = values;
    return grid;
}

bool near(double left, double right, double tolerance = 1e-10)
{
    return std::abs(left - right) <= tolerance * std::max({1.0, std::abs(left), std::abs(right)});
}

void mustThrow(const std::function<void()> &fn)
{
    bool threw = false;
    try { fn(); }
    catch (const std::invalid_argument &) { threw = true; }
    assert(threw);
}
}

int main()
{
    const ElevationGrid grid = makeGrid({
        20,20,20,20,20,
        20, 5,10, 5,20,
        20,20,20,20,20
    }, 5, 3);
    const DepressionHierarchy hierarchy = buildDepressionHierarchy(grid);
    const SpillGeometry contacts = extractSpillGeometry(grid, hierarchy);
    assert(contacts.junctions.size() == 1);
    const BasinSpillJunction &junction = contacts.junctions[0];
    assert(junction.saddle_elevation_m == 10.0);
    assert(junction.child_contacts.size() == 2);
    for (const BasinSpillContact &contact : junction.child_contacts)
    {
        assert(contact.cardinal_contact_faces != 0);
        assert(contact.cardinal_boundary_length_m > 0.0);
    }

    BroadCrestedSpillControl control;
    control.first_component_index = junction.child_contacts[0].child_basin_index;
    control.second_component_index = junction.child_contacts[1].child_basin_index;
    control.crest_elevation_m = 10.0;
    control.effective_width_m = 2.0; // EXTERNALLY PROVIDED, not inferred from DEM
    control.discharge_factor = 0.9;
    const double reference = 0.9 * 2.0 * std::sqrt(9.80665) *
                             std::pow(4.0 / 3.0, 1.5);

    const SpillDischarge forward = rateBroadCrestedSpill(junction, control, 12.0, 8.0);
    assert(forward.regime == SpillFlowRegime::FreeOverflow);
    assert(near(forward.signed_discharge_m3_s, reference));
    assert(near(forward.head_above_crest_m, 2.0));
    assert(forward.tailwater_above_crest_m == 0.0);
    const SpillDischarge backward = rateBroadCrestedSpill(junction, control, 8.0, 12.0);
    assert(backward.regime == SpillFlowRegime::FreeOverflow);
    assert(near(backward.signed_discharge_m3_s, -reference));
    assert(rateBroadCrestedSpill(junction, control, 10.0, 8.0).regime ==
           SpillFlowRegime::NoFlow);
    assert(rateBroadCrestedSpill(junction, control, 12.0, 12.0).regime ==
           SpillFlowRegime::NoFlow);
    const SpillDischarge submerged = rateBroadCrestedSpill(junction, control, 12.0, 11.0);
    assert(submerged.regime == SpillFlowRegime::SubmergedUnsupported);
    assert(std::isnan(submerged.signed_discharge_m3_s));
    assert(rateBroadCrestedSpill(junction, control, 12.0, 10.0).regime ==
           SpillFlowRegime::SubmergedUnsupported);

    const SpillReservoirCurve linear{{{0.0, 0.0}, {10.0, 1000.0}, {20.0, 2000.0}}};
    const SpillTransferStep step = stepBroadCrestedSpill(
        junction, control, linear, linear, 1200.0, 800.0, 1.0);
    assert(step.calculated);
    assert(step.initial_discharge.regime == SpillFlowRegime::FreeOverflow);
    assert(near(step.transferred_volume_m3, reference));
    assert(near(step.first_storage_after_m3 + step.second_storage_after_m3, 2000.0));
    assert(near(step.first_storage_after_m3, 1200.0 - reference));
    assert(near(step.second_storage_after_m3, 800.0 + reference));
    assert(!step.limited_by_donor_crest && !step.limited_by_tailwater_crest);

    // No time step may force either pond past the crest, even at huge dt.
    const SpillTransferStep both_limited = stepBroadCrestedSpill(
        junction, control, linear, linear, 1200.0, 800.0, 1000.0);
    assert(both_limited.calculated);
    assert(near(both_limited.transferred_volume_m3, 200.0));
    assert(near(both_limited.first_storage_after_m3, 1000.0));
    assert(near(both_limited.second_storage_after_m3, 1000.0));
    assert(both_limited.limited_by_donor_crest);
    assert(both_limited.limited_by_tailwater_crest);

    const SpillTransferStep tailwater_limited = stepBroadCrestedSpill(
        junction, control, linear, linear, 1300.0, 900.0, 1000.0);
    assert(near(tailwater_limited.transferred_volume_m3, 100.0));
    assert(!tailwater_limited.limited_by_donor_crest);
    assert(tailwater_limited.limited_by_tailwater_crest);

    const SpillTransferStep donor_limited = stepBroadCrestedSpill(
        junction, control, linear, linear, 1100.0, 500.0, 1000.0);
    assert(near(donor_limited.transferred_volume_m3, 100.0));
    assert(donor_limited.limited_by_donor_crest);
    assert(!donor_limited.limited_by_tailwater_crest);

    const SpillTransferStep reverse = stepBroadCrestedSpill(
        junction, control, linear, linear, 800.0, 1200.0, 1.0);
    assert(near(reverse.transferred_volume_m3, -reference));
    assert(near(reverse.first_storage_after_m3, 800.0 + reference));
    assert(near(reverse.second_storage_after_m3, 1200.0 - reference));

    const SpillTransferStep no_flow = stepBroadCrestedSpill(
        junction, control, linear, linear, 800.0, 900.0, 1.0);
    assert(no_flow.calculated);
    assert(no_flow.initial_discharge.regime == SpillFlowRegime::NoFlow);
    assert(no_flow.transferred_volume_m3 == 0.0);

    const SpillTransferStep unsupported = stepBroadCrestedSpill(
        junction, control, linear, linear, 1200.0, 1100.0, 1.0);
    assert(!unsupported.calculated);
    assert(unsupported.initial_discharge.regime == SpillFlowRegime::SubmergedUnsupported);
    assert(std::isnan(unsupported.initial_discharge.signed_discharge_m3_s));
    assert(unsupported.first_storage_after_m3 == 1200.0);
    assert(unsupported.second_storage_after_m3 == 1100.0);

    // Several short explicit steps: nonnegative volumes, cumulative balance.
    double left = 1500.0;
    double right = 250.0;
    for (int i = 0; i < 100; ++i)
    {
        const SpillTransferStep update = stepBroadCrestedSpill(
            junction, control, linear, linear, left, right, 0.5);
        assert(update.calculated);
        assert(update.transferred_volume_m3 >= 0.0);
        left = update.first_storage_after_m3;
        right = update.second_storage_after_m3;
        assert(left >= 1000.0 - 1e-8);
        assert(right <= 1000.0 + 1e-8);
        assert(near(left + right, 1750.0));
    }

    // Non-monotonic knots, unsuitable datum ranges and invalid calibration
    // must fail, rather than returning plausible-looking discharge values.
    BroadCrestedSpillControl invalid = control;
    invalid.effective_width_m = 0.0;
    mustThrow([&] { (void)rateBroadCrestedSpill(junction, invalid, 12.0, 8.0); });
    invalid = control;
    invalid.discharge_factor = 1.1;
    mustThrow([&] { (void)rateBroadCrestedSpill(junction, invalid, 12.0, 8.0); });
    invalid = control;
    invalid.crest_elevation_m = 9.0;
    mustThrow([&] { (void)rateBroadCrestedSpill(junction, invalid, 12.0, 8.0); });
    invalid = control;
    invalid.second_component_index = invalid.first_component_index;
    mustThrow([&] { (void)rateBroadCrestedSpill(junction, invalid, 12.0, 8.0); });
    mustThrow([&] { (void)rateBroadCrestedSpill(junction, control,
                                              std::numeric_limits<double>::quiet_NaN(), 8.0); });
    mustThrow([&] { (void)stepBroadCrestedSpill(junction, control, linear, linear,
                                               1200.0, 800.0, -1.0); });
    mustThrow([&] { (void)stepBroadCrestedSpill(junction, control,
        SpillReservoirCurve{{{0, 0}, {10, 100}, {11, 99}}}, linear,
        120.0, 800.0, 1.0); });
    mustThrow([&] { (void)stepBroadCrestedSpill(junction, control,
        SpillReservoirCurve{{{11, 0}, {20, 900}}}, linear,
        120.0, 800.0, 1.0); });
    mustThrow([&] { (void)stepBroadCrestedSpill(junction, control, linear, linear,
                                               2200.0, 800.0, 1.0); });

    // D8 diagonal-only contact MUST NOT create a rectangular spill opening.
    const ElevationGrid corners = makeGrid({
        16,16,16,16,16,
        16,10,16,10,16,
        16,16,12,16,16,
        16,16,10,16,16,
        16,16,16,16,16
    }, 5, 5);
    const DepressionHierarchy corner_hierarchy = buildDepressionHierarchy(corners);
    const SpillGeometry corner_contacts = extractSpillGeometry(corners, corner_hierarchy);
    assert(corner_contacts.junctions.size() == 1);
    assert(corner_contacts.junctions[0].child_contacts.size() == 3);
    mustThrow([&] { (void)rateBroadCrestedSpill(corner_contacts.junctions[0], control, 12.0, 8.0); });
    return 0;
}
