#ifndef AOWIS_HYDROLOGY_TEMPORAL_INFLOWS_H
#define AOWIS_HYDROLOGY_TEMPORAL_INFLOWS_H

#include <aowis/hydrology/coupled_routing.h>
#include <aowis/hydrology/redistribution.h>

#include <atomic>
#include <vector>

namespace Aowis::Hydrology
{

// Caller-specified storm timing. Fractions are nonnegative and must total one;
// the existing Stage 7 scenario supplies only *total* effective runoff, not
// these intensities. Durations must add to that scenario's event duration.
struct TemporalRainfallInterval
{
    double duration_seconds = 0.0;
    double runoff_fraction = 0.0;
};

struct TemporalRunoffForcing
{
    // Reservoir columns are in the EXACT order of the supplied reservoir list.
    // Each reservoir must correspond to one and only one depression-tree leaf.
    std::vector<CoupledInflowInterval> basin_inflows;
    // Direct runoff leaving the DEM through its raster boundary, *before*
    // basin hydraulic routing. It is not an inflow to the closed network.
    std::vector<double> direct_boundary_outflow_m3_by_interval;
    double generated_runoff_m3 = 0.0;
    double delivered_to_basins_m3 = 0.0;
    double direct_boundary_outflow_m3 = 0.0;
    double mass_balance_error_m3 = 0.0;
};

// Turn Stage 7's event-total runoff (as classified by the original-DEM dry
// D8 paths) into time-varying per-leaf inflow volumes, under an EXPLICIT,
// spatially uniform time distribution supplied by the caller. This is a
// prescribed forcing, not a travel-time model, infiltration model or derived
// runoff hydrograph. Parent merge-tree nodes cannot be treated as additional
// reservoirs alongside their children: that would double-count storage.
// Static redistribution overflow/storage must NOT be used as dynamic inflow.
TemporalRunoffForcing temporalInflowsFromRedistribution(
    const DepressionHierarchy &hierarchy,
    const RunoffRedistribution &spatial_runoff,
    const std::vector<CoupledReservoir> &reservoirs,
    const std::vector<TemporalRainfallInterval> &time_profile,
    double event_duration_seconds,
    const std::atomic<bool> *cancel_requested = nullptr);

// Convenience bridge using the original unconditioned DEM and Stage 7's saved
// runoff_volume_m3 array. Recomputes spatial dry-D8 routing; for repeat runs
// reuse the redistribution output with the function above instead.
TemporalRunoffForcing temporalInflowsFromRaster(
    const ElevationGrid &original_dem,
    const DepressionHierarchy &hierarchy,
    const std::vector<double> &runoff_volume_m3,
    const std::vector<CoupledReservoir> &reservoirs,
    const std::vector<TemporalRainfallInterval> &time_profile,
    double event_duration_seconds,
    const std::atomic<bool> *cancel_requested = nullptr);

} // namespace Aowis::Hydrology
#endif
