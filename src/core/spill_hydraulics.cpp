#include <aowis/hydrology/spill_hydraulics.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Aowis::Hydrology
{
namespace
{
constexpr double GravityMPerSecondSquared = 9.80665;

void requireFinite(double value, const char *name)
{
    if (!std::isfinite(value))
        throw std::invalid_argument(name);
}

void validateControl(const BasinSpillJunction &junction,
                     const BroadCrestedSpillControl &control)
{
    if (!std::isfinite(junction.saddle_elevation_m) ||
        junction.child_contacts.size() != 2 ||
        control.first_component_index == control.second_component_index ||
        !std::isfinite(control.crest_elevation_m) ||
        control.crest_elevation_m < junction.saddle_elevation_m ||
        !std::isfinite(control.effective_width_m) || control.effective_width_m <= 0.0 ||
        !std::isfinite(control.discharge_factor) ||
        control.discharge_factor <= 0.0 || control.discharge_factor > 1.0)
        throw std::invalid_argument("Invalid broad-crested spill control or saddle junction");

    bool first_found = false;
    bool second_found = false;
    for (const BasinSpillContact &contact : junction.child_contacts)
    {
        if (contact.cardinal_contact_faces == 0 ||
            !std::isfinite(contact.cardinal_boundary_length_m) ||
            contact.cardinal_boundary_length_m <= 0.0)
            throw std::invalid_argument("Broad-crested control needs two cardinal basin contacts");
        if (contact.child_basin_index == control.first_component_index)
            first_found = true;
        if (contact.child_basin_index == control.second_component_index)
            second_found = true;
    }
    if (!first_found || !second_found)
        throw std::invalid_argument("Spill-control basin indices do not match junction contacts");
}

void validateCurve(const SpillReservoirCurve &curve)
{
    if (curve.points.size() < 2)
        throw std::invalid_argument("Stage-storage curve requires at least two points");
    const StageStoragePoint &first = curve.points.front();
    if (!std::isfinite(first.water_level_m) || first.stored_volume_m3 != 0.0)
        throw std::invalid_argument("Stage-storage curve must begin at zero volume");
    double previous_level = first.water_level_m;
    double previous_volume = first.stored_volume_m3;
    for (std::size_t i = 1; i < curve.points.size(); ++i)
    {
        const StageStoragePoint &point = curve.points[i];
        if (!std::isfinite(point.water_level_m) ||
            !std::isfinite(point.stored_volume_m3) ||
            point.water_level_m <= previous_level ||
            point.stored_volume_m3 <= previous_volume)
            throw std::invalid_argument("Stage-storage knots must increase strictly");
        previous_level = point.water_level_m;
        previous_volume = point.stored_volume_m3;
    }
}

double interpolateByVolume(const SpillReservoirCurve &curve, double volume)
{
    if (!std::isfinite(volume) || volume < 0.0 ||
        volume > curve.points.back().stored_volume_m3)
        throw std::invalid_argument("Storage outside the supplied stage-storage curve");
    const std::vector<StageStoragePoint>::const_iterator upper =
        std::lower_bound(curve.points.begin(), curve.points.end(), volume,
            [](const StageStoragePoint &point, double value)
            { return point.stored_volume_m3 < value; });
    if (upper == curve.points.begin()) return upper->water_level_m;
    const StageStoragePoint &lower = *(upper - 1);
    const double fraction = (volume - lower.stored_volume_m3) /
                            (upper->stored_volume_m3 - lower.stored_volume_m3);
    return lower.water_level_m + fraction * (upper->water_level_m - lower.water_level_m);
}

double interpolateByLevel(const SpillReservoirCurve &curve, double level)
{
    if (!std::isfinite(level) || level < curve.points.front().water_level_m ||
        level > curve.points.back().water_level_m)
        throw std::invalid_argument("Crest outside the supplied stage-storage curve");
    const std::vector<StageStoragePoint>::const_iterator upper =
        std::lower_bound(curve.points.begin(), curve.points.end(), level,
            [](const StageStoragePoint &point, double value)
            { return point.water_level_m < value; });
    if (upper == curve.points.begin()) return upper->stored_volume_m3;
    const StageStoragePoint &lower = *(upper - 1);
    const double fraction = (level - lower.water_level_m) /
                            (upper->water_level_m - lower.water_level_m);
    return lower.stored_volume_m3 + fraction *
        (upper->stored_volume_m3 - lower.stored_volume_m3);
}
} // namespace

SpillDischarge rateBroadCrestedSpill(
    const BasinSpillJunction &junction,
    const BroadCrestedSpillControl &control,
    double first_water_level_m,
    double second_water_level_m)
{
    validateControl(junction, control);
    requireFinite(first_water_level_m, "Invalid first reservoir water level");
    requireFinite(second_water_level_m, "Invalid second reservoir water level");

    SpillDischarge result;
    const double high = std::max(first_water_level_m, second_water_level_m);
    const double low = std::min(first_water_level_m, second_water_level_m);
    result.head_above_crest_m = std::max(0.0, high - control.crest_elevation_m);
    result.tailwater_above_crest_m = std::max(0.0, low - control.crest_elevation_m);
    if (first_water_level_m == second_water_level_m || result.head_above_crest_m <= 0.0)
        return result;
    if (low >= control.crest_elevation_m)
    {
        result.regime = SpillFlowRegime::SubmergedUnsupported;
        result.signed_discharge_m3_s = std::numeric_limits<double>::quiet_NaN();
        return result;
    }

    // Rectangular critical-flow derivation: critical depth = 2H/3;
    // Q = width * sqrt(g) * (2H/3)^(3/2) * calibrated loss factor.
    const double critical_depth = (2.0 / 3.0) * result.head_above_crest_m;
    const double discharge = control.discharge_factor * control.effective_width_m *
        std::sqrt(GravityMPerSecondSquared) *
        critical_depth * std::sqrt(critical_depth);
    if (!std::isfinite(discharge))
        throw std::invalid_argument("Spillway rating exceeds numeric range");
    result.regime = SpillFlowRegime::FreeOverflow;
    result.signed_discharge_m3_s = first_water_level_m > second_water_level_m ?
        discharge : -discharge;
    return result;
}

SpillTransferStep stepBroadCrestedSpill(
    const BasinSpillJunction &junction,
    const BroadCrestedSpillControl &control,
    const SpillReservoirCurve &first_curve,
    const SpillReservoirCurve &second_curve,
    double first_storage_m3,
    double second_storage_m3,
    double duration_seconds)
{
    validateControl(junction, control);
    validateCurve(first_curve);
    validateCurve(second_curve);
    requireFinite(duration_seconds, "Invalid spill time step");
    if (duration_seconds <= 0.0)
        throw std::invalid_argument("Spill time step must be positive");
    // Fail before attempting to interpolate the initial volumes if the crest
    // is outside either curve (otherwise subsequent capacity caps are invalid).
    const double first_at_crest = interpolateByLevel(first_curve, control.crest_elevation_m);
    const double second_at_crest = interpolateByLevel(second_curve, control.crest_elevation_m);
    const double first_level = interpolateByVolume(first_curve, first_storage_m3);
    const double second_level = interpolateByVolume(second_curve, second_storage_m3);

    SpillTransferStep result;
    result.first_storage_after_m3 = first_storage_m3;
    result.second_storage_after_m3 = second_storage_m3;
    result.initial_discharge = rateBroadCrestedSpill(junction, control, first_level, second_level);
    if (result.initial_discharge.regime == SpillFlowRegime::SubmergedUnsupported)
        return result;
    result.calculated = true;
    if (result.initial_discharge.regime == SpillFlowRegime::NoFlow)
        return result;

    const bool from_first = result.initial_discharge.signed_discharge_m3_s > 0.0;
    const double donor_volume = from_first ? first_storage_m3 : second_storage_m3;
    const double recipient_volume = from_first ? second_storage_m3 : first_storage_m3;
    const double donor_at_crest = from_first ? first_at_crest : second_at_crest;
    const double recipient_at_crest = from_first ? second_at_crest : first_at_crest;
    const double donor_available = std::max(0.0, donor_volume - donor_at_crest);
    const double recipient_capacity = std::max(0.0, recipient_at_crest - recipient_volume);
    const double requested = std::abs(result.initial_discharge.signed_discharge_m3_s) * duration_seconds;
    if (!std::isfinite(requested))
        throw std::invalid_argument("Requested spill transfer exceeds numeric range");
    const double transferred = std::min({requested, donor_available, recipient_capacity});
    result.limited_by_donor_crest = requested > donor_available && donor_available <= recipient_capacity;
    result.limited_by_tailwater_crest = requested > recipient_capacity && recipient_capacity <= donor_available;
    result.transferred_volume_m3 = from_first ? transferred : -transferred;
    // Compute the recipient from its initial amount plus the transfer and the
    // donor from its initial amount minus the same transfer (one operation).
    // No independent per-basin integration errors are introduced.
    if (from_first)
    {
        result.first_storage_after_m3 -= transferred;
        result.second_storage_after_m3 += transferred;
    }
    else
    {
        result.second_storage_after_m3 -= transferred;
        result.first_storage_after_m3 += transferred;
    }
    return result;
}

} // namespace Aowis::Hydrology
