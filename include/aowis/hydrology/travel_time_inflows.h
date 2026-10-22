#ifndef AOWIS_HYDROLOGY_TRAVEL_TIME_INFLOWS_H
#define AOWIS_HYDROLOGY_TRAVEL_TIME_INFLOWS_H

#include <aowis/hydrology/temporal_inflows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Aowis::Hydrology
{

// Explicit screening assumption: every D8 edge is traversed at the same
// effective surface speed. This is not a calibrated overland/channel routing
// model; neither terrain slope nor roughness determines the speed here.
struct TravelTimeOptions
{
    double effective_velocity_m_per_s = 1.0;
    double output_interval_seconds = 60.0;
    std::uint64_t maximum_raster_cells = 10000000;
    std::size_t maximum_output_intervals = 10000;
};

struct TravelTimeRunoffForcing
{
    // Includes the post-storm tail. Basin columns follow the supplied
    // reservoirs; direct DEM-edge volumes are separate and NOT basin inflow.
    TemporalRunoffForcing forcing;
    double event_duration_seconds = 0.0;
    double maximum_source_travel_seconds = 0.0;
    double output_duration_seconds = 0.0;
    // A prescribed spatially uniform speed, not an observed travel time.
    bool constant_velocity_assumed = true;
};

// Shift each original-DEM cell's spatially uniform, piecewise-constant runoff
// time profile by its complete dry-D8 distance to a basin minimum or boundary.
// Delay is distance / effective_velocity_m_per_s; all event volume is delivered
// even after rainfall ends. The last output bin may be shorter than the others.
// Builds and cross-checks original-DEM destination totals against the static
// redistribution solver, but does NOT import static filling or overflow.
//
// Work is bounded by raster cells, time-profile intervals and output bins;
// no sub-bin hydrograph or physical wave propagation is inferred. Throws on
// inconsistent grids, topology, missing leaves, invalid parameters, exceeded
// budgets and cooperative cancellation.
TravelTimeRunoffForcing travelTimeInflowsFromRaster(
    const ElevationGrid &original_dem,
    const DepressionHierarchy &hierarchy,
    const std::vector<double> &runoff_volume_m3,
    const std::vector<CoupledReservoir> &reservoirs,
    const std::vector<TemporalRainfallInterval> &time_profile,
    double event_duration_seconds,
    const TravelTimeOptions &options = {},
    const std::atomic<bool> *cancel_requested = nullptr);

} // namespace Aowis::Hydrology

#endif
