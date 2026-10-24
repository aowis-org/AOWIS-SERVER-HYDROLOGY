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

// A ONE-WAY, free-overflow outlet from a reservoir to the external domain.
// This is an independently characterized physical control section, not an
// inferred DEM boundary cell or a BasinSpillJunction. The downstream level
// must remain STRICTLY below the crest throughout the modeled event; flooded
// or backwater-controlled boundaries require a different hydraulic law.
// No external inflow/backflow is possible through this type of outlet.
struct CoupledBoundaryOutlet
{
    std::uint64_t reservoir_component_index = 0;
    double crest_elevation_m = 0.0;
    double effective_width_m = 0.0;
    double discharge_factor = 0.0;
    double downstream_water_level_m = 0.0;
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
    // Cumulative, nonnegative discharge to the exterior, indexed in the
    // same order as the configured boundary outlets. The legacy closed-
    // network API returns an empty vector and zero cumulative outflow.
    std::vector<double> boundary_outflow_m3;
    double cumulative_boundary_outflow_m3 = 0.0;
    double total_storage_m3 = 0.0;
    double mass_balance_error_m3 = 0.0;
};

struct CoupledRoutingResult
{
    // Initial state followed by one snapshot per supplied inflow interval.
    std::vector<CoupledRoutingSnapshot> snapshots;
    std::uint64_t accepted_substeps = 0;
    std::uint64_t rejected_substeps = 0;
    // Accepted control transfers that were capped at stage equalization
    // within their substep (see routeCoupledReservoirs). Nonzero values flag
    // runs where the configured substep is coarse relative to equalization.
    std::uint64_t equalization_limited_transfers = 0;
};

// Explicit finite-volume / reservoir routing, with every control evaluated
// against the SAME start-of-substep stages and applied simultaneously.
// Each internal control's substep volume is limited to the volume that would
// equalize the stages it connects, so near-equalized reservoirs advance with
// full substeps (an explicit sqrt-head orifice law otherwise stalls). Substeps
// are still rejected and halved if they would leave a stage-storage curve,
// drain a weir donor or an outlet reservoir below its crest, or let a control
// itself reverse its head gradient. Reversal caused by inflow or other
// controls is physical and accepted.
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

// Experimental OPEN-domain variant. Boundary outlets are computed alongside
// every internal connection using the same start-of-substep water levels.
// Accepted steps conserve INITIAL + INFLOW = STORAGE + EXTERNAL OUTFLOW.
// Substeps that would lower a reservoir below an active outlet's crest are
// rejected and subdivided. Submergence/backflow, ungauged outlet geometry,
// and time-varying downstream stages are not approximated here.
CoupledRoutingResult routeCoupledReservoirsWithOutlets(
    const std::vector<CoupledReservoir> &reservoirs,
    const std::vector<CoupledConnection> &connections,
    const std::vector<CoupledBoundaryOutlet> &boundary_outlets,
    const std::vector<CoupledInflowInterval> &inflow_intervals,
    const CoupledRoutingOptions &options = {},
    const std::atomic<bool> *cancel_requested = nullptr);

} // namespace Aowis::Hydrology
#endif
