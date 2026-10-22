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


namespace
{
void validateSubmergenceRating(const BroadCrestedSubmergenceRating &rating)
{
    if (rating.points.size() < 2 || rating.points.size() > 4096)
        throw std::invalid_argument("Submergence rating requires 2..4096 knots");
    const SubmergenceRatingPoint &first = rating.points.front();
    const SubmergenceRatingPoint &last = rating.points.back();
    if (first.tailwater_ratio != 0.0 || first.discharge_multiplier != 1.0 ||
        last.tailwater_ratio != 1.0 || last.discharge_multiplier != 0.0)
        throw std::invalid_argument("Submergence rating must span (0,1) through (1,0)");
    for (std::size_t i = 1; i < rating.points.size(); ++i)
    {
        const SubmergenceRatingPoint &previous = rating.points[i - 1];
        const SubmergenceRatingPoint &point = rating.points[i];
        if (!std::isfinite(point.tailwater_ratio) ||
            !std::isfinite(point.discharge_multiplier) ||
            point.tailwater_ratio <= previous.tailwater_ratio ||
            point.tailwater_ratio > 1.0 ||
            point.discharge_multiplier < 0.0 ||
            point.discharge_multiplier > previous.discharge_multiplier ||
            (i + 1 < rating.points.size() && point.discharge_multiplier <= 0.0))
            throw std::invalid_argument("Invalid or nonmonotonic submergence rating knot");
    }
}

double submergenceMultiplier(const BroadCrestedSubmergenceRating &rating, double ratio)
{
    if (ratio <= 0.0) return 1.0;
    if (ratio >= 1.0) return 0.0;
    const std::vector<SubmergenceRatingPoint>::const_iterator upper =
        std::lower_bound(rating.points.begin(), rating.points.end(), ratio,
            [](const SubmergenceRatingPoint &point, double value)
            { return point.tailwater_ratio < value; });
    const SubmergenceRatingPoint &lower = *(upper - 1);
    const double fraction = (ratio - lower.tailwater_ratio) /
                            (upper->tailwater_ratio - lower.tailwater_ratio);
    return lower.discharge_multiplier + fraction *
        (upper->discharge_multiplier - lower.discharge_multiplier);
}

void validateOrifice(const BasinSpillJunction &junction,
                     const RectangularOrificeControl &control)
{
    if (!std::isfinite(junction.saddle_elevation_m) ||
        junction.child_contacts.size() != 2 ||
        control.first_component_index == control.second_component_index ||
        !std::isfinite(control.invert_elevation_m) ||
        !std::isfinite(control.opening_width_m) || control.opening_width_m <= 0.0 ||
        !std::isfinite(control.opening_height_m) || control.opening_height_m <= 0.0 ||
        !std::isfinite(control.discharge_factor) ||
        control.discharge_factor <= 0.0 || control.discharge_factor > 1.0 ||
        !std::isfinite(control.invert_elevation_m + control.opening_height_m) ||
        !std::isfinite(control.opening_width_m * control.opening_height_m))
        throw std::invalid_argument("Invalid fully submerged orifice control");

    bool first_found = false;
    bool second_found = false;
    for (const BasinSpillContact &contact : junction.child_contacts)
    {
        if (contact.child_basin_index == control.first_component_index)
            first_found = true;
        if (contact.child_basin_index == control.second_component_index)
            second_found = true;
    }
    if (!first_found || !second_found)
        throw std::invalid_argument("Orifice component indices do not match junction");
}

// Bisection on physically feasible volume transfers. The lower bracket is
// guaranteed to have donor water >= receiving water. If the upper bound
// cannot equalize the stages, the full bounded transfer is permissible.
double equalizationLimit(const SpillReservoirCurve &donor_curve,
                         const SpillReservoirCurve &recipient_curve,
                         double donor_storage,
                         double recipient_storage,
                         double upper_limit)
{
    if (upper_limit <= 0.0) return 0.0;
    const double donor_at_upper = interpolateByVolume(donor_curve, donor_storage - upper_limit);
    const double recipient_at_upper = interpolateByVolume(recipient_curve, recipient_storage + upper_limit);
    if (donor_at_upper >= recipient_at_upper) return upper_limit;

    double low = 0.0;
    double high = upper_limit;
    for (int iteration = 0; iteration < 80; ++iteration)
    {
        const double mid = low + (high - low) / 2.0;
        if (mid <= low || mid >= high) break;
        const double donor_level = interpolateByVolume(donor_curve, donor_storage - mid);
        const double recipient_level = interpolateByVolume(recipient_curve, recipient_storage + mid);
        if (donor_level >= recipient_level) low = mid;
        else high = mid;
    }
    return low;
}
} // namespace

SpillDischarge rateCalibratedBroadCrestedSpill(
    const BasinSpillJunction &junction,
    const BroadCrestedSpillControl &control,
    const BroadCrestedSubmergenceRating &rating,
    double first_water_level_m,
    double second_water_level_m)
{
    validateControl(junction, control);
    validateSubmergenceRating(rating);
    requireFinite(first_water_level_m, "Invalid first reservoir water level");
    requireFinite(second_water_level_m, "Invalid second reservoir water level");
    if (std::min(first_water_level_m, second_water_level_m) < control.crest_elevation_m)
        return rateBroadCrestedSpill(junction, control, first_water_level_m,
                                     second_water_level_m);

    SpillDischarge result;
    const double upstream = std::max(first_water_level_m, second_water_level_m);
    const double downstream = std::min(first_water_level_m, second_water_level_m);
    result.head_above_crest_m = std::max(0.0, upstream - control.crest_elevation_m);
    result.tailwater_above_crest_m = std::max(0.0, downstream - control.crest_elevation_m);
    if (first_water_level_m == second_water_level_m || result.head_above_crest_m <= 0.0)
        return result;

    const double critical_depth = (2.0 / 3.0) * result.head_above_crest_m;
    const double free_discharge = control.discharge_factor * control.effective_width_m *
        std::sqrt(GravityMPerSecondSquared) * critical_depth * std::sqrt(critical_depth);
    const double tailwater_ratio = result.tailwater_above_crest_m /
                                   result.head_above_crest_m;
    const double discharge = free_discharge * submergenceMultiplier(rating, tailwater_ratio);
    if (!std::isfinite(discharge))
        throw std::invalid_argument("Submerged spill rating exceeds numeric range");
    result.regime = SpillFlowRegime::SubmergedCalibrated;
    result.signed_discharge_m3_s = first_water_level_m > second_water_level_m ?
        discharge : -discharge;
    return result;
}

SpillTransferStep stepCalibratedBroadCrestedSpill(
    const BasinSpillJunction &junction,
    const BroadCrestedSpillControl &control,
    const BroadCrestedSubmergenceRating &rating,
    const SpillReservoirCurve &first_curve,
    const SpillReservoirCurve &second_curve,
    double first_storage_m3,
    double second_storage_m3,
    double duration_seconds)
{
    validateControl(junction, control);
    validateSubmergenceRating(rating);
    validateCurve(first_curve);
    validateCurve(second_curve);
    requireFinite(duration_seconds, "Invalid calibrated spill duration");
    if (duration_seconds <= 0.0)
        throw std::invalid_argument("Calibrated spill duration must be positive");
    const double first_at_crest = interpolateByLevel(first_curve, control.crest_elevation_m);
    const double second_at_crest = interpolateByLevel(second_curve, control.crest_elevation_m);
    const double first_level = interpolateByVolume(first_curve, first_storage_m3);
    const double second_level = interpolateByVolume(second_curve, second_storage_m3);

    SpillTransferStep result;
    result.first_storage_after_m3 = first_storage_m3;
    result.second_storage_after_m3 = second_storage_m3;
    result.initial_discharge = rateCalibratedBroadCrestedSpill(
        junction, control, rating, first_level, second_level);
    result.calculated = true;
    if (result.initial_discharge.regime == SpillFlowRegime::NoFlow)
        return result;

    const bool from_first = result.initial_discharge.signed_discharge_m3_s > 0.0;
    const SpillReservoirCurve &donor_curve = from_first ? first_curve : second_curve;
    const SpillReservoirCurve &recipient_curve = from_first ? second_curve : first_curve;
    const double donor_storage = from_first ? first_storage_m3 : second_storage_m3;
    const double recipient_storage = from_first ? second_storage_m3 : first_storage_m3;
    const double donor_crest_storage = from_first ? first_at_crest : second_at_crest;
    const double donor_available = std::max(0.0, donor_storage - donor_crest_storage);
    const double recipient_capacity =
        std::max(0.0, recipient_curve.points.back().stored_volume_m3 - recipient_storage);
    const double capacity_bound = std::min(donor_available, recipient_capacity);
    const double equilibrium_bound = equalizationLimit(
        donor_curve, recipient_curve, donor_storage, recipient_storage, capacity_bound);
    const double requested = std::abs(result.initial_discharge.signed_discharge_m3_s) * duration_seconds;
    if (!std::isfinite(requested))
        throw std::invalid_argument("Requested calibrated transfer exceeds numeric range");
    const double transferred = std::min(requested, equilibrium_bound);
    result.limited_by_donor_crest = requested > transferred &&
        donor_available <= recipient_capacity &&
        donor_available <= equilibrium_bound;
    result.limited_by_recipient_curve = requested > transferred &&
        recipient_capacity <= donor_available &&
        recipient_capacity <= equilibrium_bound;
    result.limited_by_level_equalization = requested > transferred &&
        equilibrium_bound < capacity_bound;
    result.transferred_volume_m3 = from_first ? transferred : -transferred;
    if (from_first)
    {
        result.first_storage_after_m3 -= transferred;
        result.second_storage_after_m3 += transferred;
    }
    else
    {
        result.first_storage_after_m3 += transferred;
        result.second_storage_after_m3 -= transferred;
    }
    return result;
}

SpillDischarge rateFullySubmergedOrifice(
    const BasinSpillJunction &junction,
    const RectangularOrificeControl &control,
    double first_water_level_m,
    double second_water_level_m)
{
    validateOrifice(junction, control);
    requireFinite(first_water_level_m, "Invalid first orifice water level");
    requireFinite(second_water_level_m, "Invalid second orifice water level");
    SpillDischarge result;
    if (first_water_level_m == second_water_level_m ||
        (first_water_level_m <= control.invert_elevation_m &&
         second_water_level_m <= control.invert_elevation_m))
        return result;

    const double crown = control.invert_elevation_m + control.opening_height_m;
    if (std::min(first_water_level_m, second_water_level_m) < crown)
    {
        result.regime = SpillFlowRegime::PartiallySubmergedUnsupported;
        result.signed_discharge_m3_s = std::numeric_limits<double>::quiet_NaN();
        return result;
    }
    const double difference = std::abs(first_water_level_m - second_water_level_m);
    const double magnitude = control.discharge_factor *
        (control.opening_width_m * control.opening_height_m) *
        std::sqrt(2.0 * GravityMPerSecondSquared * difference);
    if (!std::isfinite(magnitude))
        throw std::invalid_argument("Fully submerged orifice rating exceeds numeric range");
    result.regime = SpillFlowRegime::FullySubmergedOrifice;
    // For this orifice only, reuse the discharge diagnostic head fields as
    // water depth above the opening CROWN (rather than a weir crest).
    result.head_above_crest_m = std::max(0.0, std::max(first_water_level_m, second_water_level_m) - crown);
    result.tailwater_above_crest_m = std::max(0.0, std::min(first_water_level_m, second_water_level_m) - crown);
    result.signed_discharge_m3_s = first_water_level_m > second_water_level_m ?
        magnitude : -magnitude;
    return result;
}

} // namespace Aowis::Hydrology
