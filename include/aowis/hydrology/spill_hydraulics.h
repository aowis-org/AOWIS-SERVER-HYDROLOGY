#ifndef AOWIS_HYDROLOGY_SPILL_HYDRAULICS_H
#define AOWIS_HYDROLOGY_SPILL_HYDRAULICS_H

#include <aowis/hydrology/spill_geometry.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace Aowis::Hydrology
{

// An explicitly specified rectangular broad-crested free-overflow control.
// Both basin indices refer to BELOW-SADDLE components in the given junction,
// not necessarily direct children of its parent merge node. The hydraulic
// width and loss coefficient MUST be supplied from external, defensible data;
// raster cardinal-contact length is deliberately never used as a width.
struct BroadCrestedSpillControl
{
    std::uint64_t first_component_index = 0;
    std::uint64_t second_component_index = 0;
    double crest_elevation_m = std::numeric_limits<double>::quiet_NaN();
    double effective_width_m = std::numeric_limits<double>::quiet_NaN();
    // Dimensionless multiplier on the ideal rectangular critical-flow rate,
    // NOT the dimensional C in Q = C * width * head^(3/2).
    double discharge_factor = std::numeric_limits<double>::quiet_NaN();
};

enum class SpillFlowRegime
{
    NoFlow,
    FreeOverflow,
    SubmergedUnsupported
};

struct SpillDischarge
{
    SpillFlowRegime regime = SpillFlowRegime::NoFlow;
    // Positive from first_component_index to second_component_index.
    // NaN when the control is submerged (no rate is available).
    double signed_discharge_m3_s = 0.0;
    double head_above_crest_m = 0.0;
    double tailwater_above_crest_m = 0.0;
};

// Validate the section against DEM-derived basin contacts. Only a TWO-way
// junction where each component has at least one cardinal face can currently
// be represented by this single broad-crested control. Multiway saddles and
// corner-only raster connectivity require more detailed control geometry.
// Upstream/downstream orientation follows water level, not component index.
// Free-overflow computation uses Q = factor * width * sqrt(g) *
// (2 * H / 3)^(3/2), with H approximated by water depth above the crest.
// If both water surfaces exceed the crest, the method refuses to estimate
// submerged flow (it does NOT silently apply a free-flow rate).
SpillDischarge rateBroadCrestedSpill(
    const BasinSpillJunction &junction,
    const BroadCrestedSpillControl &control,
    double first_water_level_m,
    double second_water_level_m);

struct StageStoragePoint
{
    double water_level_m = 0.0;
    double stored_volume_m3 = 0.0;
};

// A measured/modelled basin stage-storage curve in the SAME elevation datum
// as the spillway crest. Levels and volumes must increase strictly; the first
// point is zero storage. This first primitive does NOT manufacture above-saddle
// stage-storage relationships from the static depression hierarchy.
struct SpillReservoirCurve
{
    std::vector<StageStoragePoint> points;
};

struct SpillTransferStep
{
    SpillDischarge initial_discharge;
    double first_storage_after_m3 = 0.0;
    double second_storage_after_m3 = 0.0;
    // Signed, positive when moved from the first reservoir to the second.
    double transferred_volume_m3 = 0.0;
    // If false, NO transfer was applied: submerged flow is unsupported.
    bool calculated = false;
    bool limited_by_donor_crest = false;
    bool limited_by_tailwater_crest = false;
};

// One forward-Euler free-flow step, WITHOUT rainfall inflow, evaporation or
// downstream/backwater dynamics. Transfers are capped so the donor does not
// fall BELOW the crest and the receiver does not rise ABOVE the crest. The
// step preserves volume exactly to rounding; limits are NOT a model of
// submerged flow. A submerged start returns calculated=false with unchanged
// storage and NaN discharge. Large time steps have first-order error.
SpillTransferStep stepBroadCrestedSpill(
    const BasinSpillJunction &junction,
    const BroadCrestedSpillControl &control,
    const SpillReservoirCurve &first_curve,
    const SpillReservoirCurve &second_curve,
    double first_storage_m3,
    double second_storage_m3,
    double duration_seconds);

} // namespace Aowis::Hydrology
#endif
