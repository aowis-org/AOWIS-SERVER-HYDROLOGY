#ifndef AOWIS_HYDROLOGY_ANALYSIS_H
#define AOWIS_HYDROLOGY_ANALYSIS_H

#include <aowis/hydrology/geometry.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace Aowis::Hydrology
{

// Terrain is stored separately from georeferencing. Both WGS84 geographic
// and WGS84 UTM grids can be processed without resampling the elevations.
struct ElevationGrid
{
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
    RasterGeometry geometry;
    std::vector<float> elevations_m;
};

struct AnalysisOptions
{
    // Filled terrain establishes potential boundary-connected drainage.
    // When false, true closed basins remain sinks; natural flat plateaus
    // can still route toward a lower neighboring cell.
    bool fill_depressions = true;
    bool reject_interior_nodata = true;

    // Optional conservative preconditioning: try local, bounded cuts through
    // short barriers draining closed pits. Independent of whether remaining
    // depressions are subsequently filled. Disabled by default for GLO-30.
    bool breach_depressions = false;
    double maximum_breach_cut_depth_m = 3.0;
    std::uint32_t maximum_breach_path_cells = 8;
    double maximum_breach_excavation_m3 = 100000.0;
    std::uint32_t maximum_breach_candidates = 2000;
};

// Downstream is a linear cell index, or -1 for boundary outlets, true sinks,
// and no-data cells. Arrays contain one entry per input cell.
struct AnalysisResult
{
    ElevationGrid source;
    // Routing terrain: physical fill level if enabled, otherwise source DEM.
    // Never includes synthetic epsilon slopes (flat routing is topological).
    std::vector<float> conditioned_elevations_m;
    // Potential fill depth for every cell, regardless of routing policy.
    // NaN at missing cells. Derived from *unmodified* terrain; not a
    // prediction of actual flooding or basin water level.
    std::vector<float> potential_fill_depth_m;
    std::vector<std::int64_t> downstream;
    std::vector<std::uint64_t> upstream_cell_count;
    std::vector<double> upstream_area_m2;
    std::uint64_t conditioned_cell_count = 0;
    float maximum_fill_m = 0.0f;
    // Cut-depth diagnostics only for accepted constrained breaches. Cuts are
    // negative elevation changes, and are not inferred rainfall volumes.
    std::vector<float> breach_cut_depth_m;
    std::uint64_t breached_cell_count = 0;
    std::uint32_t accepted_breach_paths = 0;
    std::uint32_t rejected_breach_candidates = 0;
    double breach_excavation_m3 = 0.0;
    float maximum_breach_cut_m = 0.0f;
    std::uint64_t potential_fill_cell_count = 0;
    double potential_fill_volume_m3 = 0.0;
    float maximum_potential_fill_m = 0.0f;
};

// Throws std::invalid_argument on malformed data and std::runtime_error on
// internal graph errors. Immutable input, deterministic result.
AnalysisResult analyze(const ElevationGrid &grid,
                       const AnalysisOptions &options = {},
                       const std::atomic<bool> *cancel_requested = nullptr);

// Returns one byte per cell; 1 for contributing upstream cells (including the
// selected outlet), zero otherwise. This operates on the complete analysis
// domain, not only the viewport or the user's visible polygon.
std::vector<std::uint8_t> delineateCatchment(const AnalysisResult &result,
                                             std::uint64_t outlet_index,
                                             const std::atomic<bool> *cancel_requested = nullptr);

// Centre-of-cell for an index in the grid, in the grid's native CRS.
void cellCoordinate(const ElevationGrid &grid, std::uint64_t index,
                    double *x, double *y);

} // namespace Aowis::Hydrology
#endif
