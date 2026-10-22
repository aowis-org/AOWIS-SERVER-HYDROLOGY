#ifndef AOWIS_HYDROLOGY_COUPLED_ROUTING_H
#define AOWIS_HYDROLOGY_COUPLED_ROUTING_H

#include <aowis/hydrology/spill_hydraulics.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Aowis::Hydrology
{

// Reservoir IDs refer to independent hydraulic control volumes, not to
// parent/child depression-tree volumes (which would double-count water).
// The stage-storage curve is supplied by the caller and must cover all stages
// reached during the simulation, including the control crest/crown.
struct CoupledReservoir
{
    std::uint64_t component_index = 0;
    SpillReservoirCurve curve;
    double initial_storage_m3 = 0.0;
};

enum class CoupledControlType
{
    CalibratedBroadCrestedWeir,
    FullySubmergedRectangularOrifice
};

struct CoupledConnection
{
    CoupledControlType type = CoupledControlType::CalibratedBroadCrestedWeir;
    // An externally validated, DEM-derived junction; see spill_geometry.h.
    BasinSpillJunction junction;
    // For a weir: the complete explicit section + tailwater rating.
    BroadCrestedSpillControl weir;
    BroadCrestedSubmergenceRating submergence;
    // For an orifice: surveyed invert, dimensions and discharge factor.
    RectangularOrificeControl orifice;
};

// Each interval has a prescribed nonnegative ADDITIONAL volume for each basin.
// The volume arrives uniformly during that interval. This is NOT derived from
// a total rainfall depth or a hydrograph by this solver.
struct CoupledInflowInterval
{
    double duration_seconds = 0.0;
    std::vector<double> inflow_volume_m3;
};

struct CoupledRoutingOptions
{
    double maximum_substep_seconds = 1.0;
    std::uint64_t maximum_substeps = 1000000;
    std::uint64_t maximum_output_intervals = 10000;
};

struct CoupledRoutingSnapshot
{
    double elapsed_seconds = 0.0;
    std::vector<double> storage_m3;
    std::vector<double> water_level_m;
    // Signed cumulative volume through each connection in its declared
    // orientation, from the initial state through this interval boundary.
    std::vector<double> connection_transfer_m3;
    double cumulative_inflow_m3 = 0.0;
    double total_storage_m3 = 0.0;
    double mass_balance_error_m3 = 0.0;
};

struct CoupledRoutingResult
{
    // Initial state followed by one snapshot per supplied inflow interval.
    std::vector<CoupledRoutingSnapshot> snapshots;
    std::uint64_t accepted_substeps = 0;
    std::uint64_t rejected_substeps = 0;
};

// Explicit finite-volume / reservoir routing, with every control evaluated
// against the SAME start-of-substep stages and applied simultaneously.
// Rejected substeps are halved if they would leave a stage-storage curve,
// drain a weir donor below its crest, or reverse a control's head gradient.
// Unsupported partial orifice regimes fail explicitly; no guessed hydraulics.
// Closed network only (no boundary outflow or negative inflow). Throws for
// invalid parameters, excessive work, unsupported regimes or cancellation.
// No mutation of persisted static redistribution or inundation results.
CoupledRoutingResult routeCoupledReservoirs(
    const std::vector<CoupledReservoir> &reservoirs,
    const std::vector<CoupledConnection> &connections,
    const std::vector<CoupledInflowInterval> &inflow_intervals,
    const CoupledRoutingOptions &options = {},
    const std::atomic<bool> *cancel_requested = nullptr);

} // namespace Aowis::Hydrology
#endif
