#ifndef AOWIS_HYDROLOGY_SPATIAL_QUERIES_H
#define AOWIS_HYDROLOGY_SPATIAL_QUERIES_H

#include <aowis/hydrology/flow_network.h>

#include <cstdint>
#include <vector>

namespace Aowis::Hydrology
{

struct GridVertex
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
};

// A polygon has one exterior ring followed by zero or more holes. Vertices
// lie on DEM cell edges, not cell centres; all rings close explicitly.
struct WatershedPolygon
{
    std::vector<std::vector<GridVertex>> rings;
};

struct WatershedBoundary
{
    std::vector<WatershedPolygon> polygons;
    std::uint64_t contributing_cells = 0;
    double contributing_area_m2 = 0.0;
    bool touches_domain_boundary = false;
};

// Polygonize a D8 catchment mask without interpolating its topographic extent.
// Disconnected / diagonal-touching cell sets become independent polygons.
// The vertex budget avoids exporting an unbounded GeoJSON boundary.
WatershedBoundary polygonizeCatchment(const ElevationGrid &grid,
    const std::vector<std::uint8_t> &catchment_mask,
    std::uint64_t maximum_boundary_edges = 100000);

// Coordinates are WGS84 longitude/latitude. Polygons may contain holes;
// their input rings do not need a particular winding order.
using GeographicRing = std::vector<MapCoordinate>;
struct GeographicPolygon
{
    GeographicRing exterior;
    std::vector<GeographicRing> holes;
};

struct DrainageWindow
{
    double west = -180.0;
    double south = -90.0;
    double east = 180.0;
    double north = 90.0;
};

struct PolygonDrainageResult
{
    ChannelNetwork network;
    std::uint64_t source_cells = 0;
    std::uint64_t downstream_affected_cells = 0;
    std::uint64_t affected_boundary_cells = 0;
    bool clipped_to_window = false;
};

// Extract only channels whose cell centres lie in a WGS84 window. The
// original full-domain accumulation values are retained, but the stream
// topology/order is recalculated for this clipped subset.
ChannelNetwork extractWindowChannelNetwork(
    const AnalysisResult &result, double minimum_upstream_area_m2,
    const DrainageWindow &window,
    std::uint64_t maximum_selected_channel_cells = 25000,
    std::uint32_t maximum_segments = 2000);

// This is a TOPOLOGICAL downstream influence query, not a rainfall/runoff
// volume computation. It includes cells reached by any selected source cell,
// and selects channels using the full precomputed upstream-area raster.
// Stream ordering in a clipped region is local to the resulting subgraph.
PolygonDrainageResult downstreamNetworkFromPolygon(
    const AnalysisResult &result,
    const std::vector<GeographicPolygon> &polygons,
    double minimum_upstream_area_m2,
    const DrainageWindow &window = {},
    std::uint64_t maximum_source_cells = 100000,
    std::uint64_t maximum_affected_cells = 250000,
    std::uint64_t maximum_selected_channel_cells = 25000,
    std::uint32_t maximum_segments = 2000);

// Reader-based variants for persisted graphs: identical results, but memory
// proportional to the selected/affected cells (bounded by the budgets above)
// instead of tens of bytes per grid cell. The window query reads the area
// raster once; the polygon query reads areas only inside the polygons'
// bounding box (geographic grids) and links only along the traced paths.
ChannelNetwork extractWindowChannelNetwork(
    const RasterGeometry &geometry, std::uint32_t columns, std::uint32_t rows,
    const DownstreamReader &downstream, const UpstreamAreaReader &upstream_area,
    double minimum_upstream_area_m2, const DrainageWindow &window,
    std::uint64_t maximum_selected_channel_cells = 25000,
    std::uint32_t maximum_segments = 2000);

PolygonDrainageResult downstreamNetworkFromPolygon(
    const RasterGeometry &geometry, std::uint32_t columns, std::uint32_t rows,
    const DownstreamReader &downstream, const UpstreamAreaReader &upstream_area,
    const std::vector<GeographicPolygon> &polygons,
    double minimum_upstream_area_m2,
    const DrainageWindow &window = {},
    std::uint64_t maximum_source_cells = 100000,
    std::uint64_t maximum_affected_cells = 250000,
    std::uint64_t maximum_selected_channel_cells = 25000,
    std::uint32_t maximum_segments = 2000);

// Cell-edge vertex converted to WGS84. Do not use pixelCentreWgs84 here.
MapCoordinate gridVertexWgs84(const RasterGeometry &geometry,
                              const GridVertex &vertex);

} // namespace Aowis::Hydrology
#endif
