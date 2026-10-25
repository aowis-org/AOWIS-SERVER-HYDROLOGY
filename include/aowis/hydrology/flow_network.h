#ifndef AOWIS_HYDROLOGY_FLOW_NETWORK_H
#define AOWIS_HYDROLOGY_FLOW_NETWORK_H

#include <aowis/hydrology/analysis.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace Aowis::Hydrology
{

// Linear indexes are relative to one complete analysis grid; they must never
// be interpreted as globally unique terrain or watershed identifiers.
enum class TraceTermination : std::uint8_t
{
    DomainOutlet,
    InternalSink,
    DistanceLimit,
    PointLimit
};

struct TracePoint
{
    std::uint64_t cell_index = 0;
    double distance_from_start_m = 0.0;
    float conditioned_elevation_m = 0.0f;
};

struct FlowTrace
{
    std::vector<TracePoint> points;
    TraceTermination termination = TraceTermination::InternalSink;
    double traced_length_m = 0.0;
    double net_slope = 0.0; // (start height - end height) / traced length
};

using DownstreamReader = std::function<std::int64_t(std::uint64_t)>;
using ElevationReader = std::function<float(std::uint64_t)>;

// The reader-based overload permits O(path length) disk access to persisted
// flow graphs, without loading a full metropolitan raster into memory.
// Distances are computed along cell-centre segments on the stored raster CRS.
FlowTrace traceDownstream(const RasterGeometry &geometry,
                          std::uint32_t columns, std::uint32_t rows,
                          std::uint64_t start_cell, double maximum_distance_m,
                          std::uint32_t maximum_points,
                          const DownstreamReader &downstream,
                          const ElevationReader &elevation);
FlowTrace traceDownstream(const AnalysisResult &result,
                          std::uint64_t start_cell, double maximum_distance_m,
                          std::uint32_t maximum_points = 10000);

struct ChannelNode
{
    std::uint64_t cell_index = 0;
    std::uint8_t incoming_channels = 0;
    std::uint16_t strahler_order = 1;
};

struct ChannelSegment
{
    std::uint64_t id = 0; // stable within this analysis: upstream node index
    std::uint64_t from_cell = 0;
    std::uint64_t to_cell = 0;
    std::uint16_t strahler_order = 1;
    double length_m = 0.0;
    double upstream_area_at_end_m2 = 0.0;
    std::vector<std::uint64_t> cells; // ordered upstream -> downstream, includes both endpoints
};

struct ChannelNetwork
{
    double threshold_m2 = 0.0;
    std::uint64_t selected_cells = 0;
    std::vector<ChannelNode> nodes;
    std::vector<ChannelSegment> segments;
};

// A channel cell is selected when its upstream contributing area meets the
// threshold. A segment runs between channel sources, confluences or outlets.
// Strahler order is computed from selected-channel topology, not from every
// D8 hillslope cell. Resource limits prevent unexpectedly enormous responses.
ChannelNetwork extractChannelNetwork(const AnalysisResult &result,
                                     double minimum_upstream_area_m2,
                                     std::uint64_t maximum_selected_cells = 25000,
                                     std::uint32_t maximum_segments = 2000);

// Random-access reader of the persisted contributing-area raster (m2).
using UpstreamAreaReader = std::function<double(std::uint64_t)>;

// Reader-based extraction for persisted graphs. Memory is proportional to the
// SELECTED channel cells (bounded by maximum_selected_cells), not to the
// grid: one sequential pass reads the area raster, and downstream links are
// read only for selected cells. Results are identical to the overload above.
ChannelNetwork extractChannelNetwork(const RasterGeometry &geometry,
                                     std::uint32_t columns, std::uint32_t rows,
                                     const DownstreamReader &downstream,
                                     const UpstreamAreaReader &upstream_area,
                                     double minimum_upstream_area_m2,
                                     std::uint64_t maximum_selected_cells = 25000,
                                     std::uint32_t maximum_segments = 2000);

// Build the network for an explicit cell selection: strictly ascending cell
// indices whose areas (parallel vector) are the full-domain contributing
// areas. Links leaving the selection end a segment. Used by the window and
// polygon queries; callers enforce their own selection budget.
ChannelNetwork channelNetworkFromCells(const RasterGeometry &geometry,
                                       std::uint32_t columns, std::uint32_t rows,
                                       const std::vector<std::uint64_t> &selected_cells,
                                       const std::vector<double> &selected_areas_m2,
                                       const DownstreamReader &downstream,
                                       double threshold_m2,
                                       std::uint32_t maximum_segments);

} // namespace Aowis::Hydrology
#endif
