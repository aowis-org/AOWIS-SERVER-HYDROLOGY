#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/spill_hydraulics.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

using namespace Aowis::Hydrology;

namespace
{
bool near(double left, double right, double tolerance = 1e-9)
{
    return std::abs(left - right) <= tolerance *
        std::max({1.0, std::abs(left), std::abs(right)});
}

void mustThrow(const std::function<void()> &call)
{
    bool caught = false;
    try { call(); }
    catch (const std::invalid_argument &) { caught = true; }
    assert(caught);
}

BasinSpillJunction junctionFixture()
{
    ElevationGrid grid;
    grid.columns = 5;
    grid.rows = 3;
    grid.geometry.origin_x = 8.0;
    grid.geometry.origin_y = 50.0;
    grid.geometry.pixel_width = 0.001;
    grid.geometry.pixel_height = 0.001;
    grid.elevations_m = {
        20,20,20,20,20,
        20, 5,10, 5,20,
        20,20,20,20,20
    };
    const DepressionHierarchy hierarchy = buildDepressionHierarchy(grid);
    const SpillGeometry geometry = extractSpillGeometry(grid, hierarchy);
    assert(geometry.junctions.size() == 1);
    assert(geometry.junctions[0].child_contacts.size() == 2);
    return geometry.junctions.front();
}
}

int main()
{
    const BasinSpillJunction junction = junctionFixture();
    BroadCrestedSpillControl control;
    control.first_component_index = junction.child_contacts[0].child_basin_index;
    control.second_component_index = junction.child_contacts[1].child_basin_index;
    control.crest_elevation_m = 10.0;
    control.effective_width_m = 2.0;
    control.discharge_factor = 0.9;

    BroadCrestedSubmergenceRating rating;
    rating.points = {{0.0, 1.0}, {0.5, 0.8}, {0.9, 0.3}, {1.0, 0.0}};
    const double free_rate = 0.9 * 2.0 * std::sqrt(9.80665) *
        std::pow(4.0 / 3.0, 1.5);

    const SpillDischarge free = rateCalibratedBroadCrestedSpill(
        junction, control, rating, 12.0, 8.0);
    assert(free.regime == SpillFlowRegime::FreeOverflow);
    assert(near(free.signed_discharge_m3_s, free_rate));
    const SpillDischarge calibrated = rateCalibratedBroadCrestedSpill(
        junction, control, rating, 12.0, 11.0);
    assert(calibrated.regime == SpillFlowRegime::SubmergedCalibrated);
    assert(near(calibrated.head_above_crest_m, 2.0));
    assert(near(calibrated.tailwater_above_crest_m, 1.0));
    assert(near(calibrated.signed_discharge_m3_s, 0.8 * free_rate));
    const SpillDischarge middle = rateCalibratedBroadCrestedSpill(
        junction, control, rating, 12.0, 11.5);
    assert(near(middle.signed_discharge_m3_s, 0.4875 * free_rate));
    const SpillDischarge reversed = rateCalibratedBroadCrestedSpill(
        junction, control, rating, 11.0, 12.0);
    assert(near(reversed.signed_discharge_m3_s, -calibrated.signed_discharge_m3_s));
    assert(rateCalibratedBroadCrestedSpill(junction, control, rating, 12.0, 12.0).regime ==
           SpillFlowRegime::NoFlow);
    assert(rateBroadCrestedSpill(junction, control, 12.0, 11.0).regime ==
           SpillFlowRegime::SubmergedUnsupported); // legacy API is unchanged

    const SpillReservoirCurve curve{{{0.0, 0.0}, {10.0, 1000.0}, {20.0, 2000.0}}};
    const SpillTransferStep short_step = stepCalibratedBroadCrestedSpill(
        junction, control, rating, curve, curve, 1200.0, 1100.0, 0.1);
    assert(short_step.calculated);
    assert(short_step.initial_discharge.regime == SpillFlowRegime::SubmergedCalibrated);
    assert(near(short_step.transferred_volume_m3, calibrated.signed_discharge_m3_s * 0.1));
    assert(near(short_step.first_storage_after_m3 + short_step.second_storage_after_m3,
                2300.0));
    assert(short_step.first_storage_after_m3 > short_step.second_storage_after_m3);

    const SpillTransferStep huge_step = stepCalibratedBroadCrestedSpill(
        junction, control, rating, curve, curve, 1300.0, 1100.0, 1e6);
    assert(huge_step.calculated);
    assert(near(huge_step.first_storage_after_m3, 1200.0, 1e-8));
    assert(near(huge_step.second_storage_after_m3, 1200.0, 1e-8));
    assert(huge_step.limited_by_level_equalization);
    assert(near(huge_step.transferred_volume_m3, 100.0, 1e-8));

    const SpillTransferStep donor_limited = stepCalibratedBroadCrestedSpill(
        junction, control, rating, curve, curve, 1100.0, 500.0, 1e6);
    assert(donor_limited.calculated && donor_limited.limited_by_donor_crest);
    assert(near(donor_limited.first_storage_after_m3, 1000.0));
    assert(near(donor_limited.second_storage_after_m3, 600.0));

    const SpillReservoirCurve small_receiver{{{0.0, 0.0}, {10.0, 1000.0}, {10.5, 1050.0}}};
    const SpillTransferStep recipient_limited = stepCalibratedBroadCrestedSpill(
        junction, control, rating, curve, small_receiver, 1300.0, 1020.0, 1e6);
    assert(recipient_limited.calculated && recipient_limited.limited_by_recipient_curve);
    assert(near(recipient_limited.transferred_volume_m3, 30.0));
    assert(near(recipient_limited.second_storage_after_m3, 1050.0));

    const SpillTransferStep dry = stepCalibratedBroadCrestedSpill(
        junction, control, rating, curve, curve, 700.0, 800.0, 1.0);
    assert(dry.calculated && dry.initial_discharge.regime == SpillFlowRegime::NoFlow);
    assert(dry.transferred_volume_m3 == 0.0);

    // Stress test: neither the sign of the head gradient nor total water may
    // be reversed/changed by a single (possibly very large) transfer step.
    std::mt19937_64 generator(20261022);
    std::uniform_real_distribution<double> volume_distribution(1000.0, 2000.0);
    std::uniform_real_distribution<double> seconds_distribution(0.001, 30000.0);
    for (int i = 0; i < 1000; ++i)
    {
        const double left = volume_distribution(generator);
        const double right = volume_distribution(generator);
        const SpillTransferStep step = stepCalibratedBroadCrestedSpill(
            junction, control, rating, curve, curve, left, right,
            seconds_distribution(generator));
        assert(step.calculated);
        assert(step.first_storage_after_m3 >= 1000.0 - 1e-7);
        assert(step.second_storage_after_m3 >= 1000.0 - 1e-7);
        assert(step.first_storage_after_m3 <= 2000.0 + 1e-7);
        assert(step.second_storage_after_m3 <= 2000.0 + 1e-7);
        assert(near(step.first_storage_after_m3 + step.second_storage_after_m3,
                    left + right));
        assert((left > right) ?
            step.first_storage_after_m3 >= step.second_storage_after_m3 - 1e-8 :
            step.first_storage_after_m3 <= step.second_storage_after_m3 + 1e-8);
        assert((left > right) ? step.transferred_volume_m3 >= 0.0 :
               step.transferred_volume_m3 <= 0.0);
    }

    BroadCrestedSubmergenceRating bad_rating = rating;
    bad_rating.points[1].discharge_multiplier = 1.1;
    mustThrow([&] { (void)rateCalibratedBroadCrestedSpill(
        junction, control, bad_rating, 12.0, 11.0); });
    bad_rating = rating;
    bad_rating.points[2].tailwater_ratio = 0.4;
    mustThrow([&] { (void)rateCalibratedBroadCrestedSpill(
        junction, control, bad_rating, 12.0, 11.0); });
    bad_rating = rating;
    bad_rating.points.back().discharge_multiplier = 0.2;
    mustThrow([&] { (void)rateCalibratedBroadCrestedSpill(
        junction, control, bad_rating, 12.0, 11.0); });
    bad_rating = rating;
    bad_rating.points[1].discharge_multiplier = std::numeric_limits<double>::quiet_NaN();
    mustThrow([&] { (void)rateCalibratedBroadCrestedSpill(
        junction, control, bad_rating, 12.0, 11.0); });
    mustThrow([&] { (void)stepCalibratedBroadCrestedSpill(
        junction, control, rating, curve, curve, 1200.0, 1100.0, 0.0); });

    RectangularOrificeControl opening;
    opening.first_component_index = control.first_component_index;
    opening.second_component_index = control.second_component_index;
    opening.invert_elevation_m = 4.0; // BELOW the DEM saddle: external engineered opening
    opening.opening_width_m = 2.0;
    opening.opening_height_m = 0.5;
    opening.discharge_factor = 0.6;
    const double reference_orifice = 0.6 * 1.0 * std::sqrt(2.0 * 9.80665);
    const SpillDischarge orifice = rateFullySubmergedOrifice(junction, opening, 12.0, 11.0);
    assert(orifice.regime == SpillFlowRegime::FullySubmergedOrifice);
    assert(near(orifice.signed_discharge_m3_s, reference_orifice));
    assert(near(rateFullySubmergedOrifice(junction, opening, 11.0, 12.0).
                signed_discharge_m3_s, -reference_orifice));
    assert(rateFullySubmergedOrifice(junction, opening, 11.0, 11.0).regime ==
           SpillFlowRegime::NoFlow);
    assert(rateFullySubmergedOrifice(junction, opening, 3.0, 3.5).regime ==
           SpillFlowRegime::NoFlow);
    const SpillDischarge partial = rateFullySubmergedOrifice(junction, opening, 12.0, 4.3);
    assert(partial.regime == SpillFlowRegime::PartiallySubmergedUnsupported);
    assert(std::isnan(partial.signed_discharge_m3_s));
    const SpillDischarge crown = rateFullySubmergedOrifice(junction, opening, 12.0, 4.5);
    assert(crown.regime == SpillFlowRegime::FullySubmergedOrifice);

    RectangularOrificeControl bad_opening = opening;
    bad_opening.opening_height_m = -1.0;
    mustThrow([&] { (void)rateFullySubmergedOrifice(junction, bad_opening, 12.0, 11.0); });
    bad_opening = opening;
    bad_opening.discharge_factor = 1.01;
    mustThrow([&] { (void)rateFullySubmergedOrifice(junction, bad_opening, 12.0, 11.0); });
    bad_opening = opening;
    bad_opening.second_component_index = bad_opening.first_component_index;
    mustThrow([&] { (void)rateFullySubmergedOrifice(junction, bad_opening, 12.0, 11.0); });
    mustThrow([&] { (void)rateFullySubmergedOrifice(junction, opening,
        std::numeric_limits<double>::infinity(), 11.0); });

    return 0;
}
