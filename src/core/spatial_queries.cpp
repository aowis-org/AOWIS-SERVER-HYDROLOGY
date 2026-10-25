#include <aowis/hydrology/spatial_queries.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace Aowis::Hydrology
{
namespace
{
struct Edge
{
    std::uint64_t from = 0;
    std::uint64_t to = 0;
    std::uint8_t direction = 0; // E,S,W,N in screen coordinates
    bool consumed = false;
};

using Outgoing = std::array<std::int64_t, 4>;

std::uint64_t vertexKey(std::uint64_t x, std::uint64_t y, std::uint32_t columns)
{
    return y * (std::uint64_t(columns) + 1) + x;
}

GridVertex decodeVertex(std::uint64_t key, std::uint32_t columns)
{
    return {std::uint32_t(key % (std::uint64_t(columns) + 1)),
            std::uint32_t(key / (std::uint64_t(columns) + 1))};
}

long double signedArea(const std::vector<GridVertex> &ring)
{
    long double doubled_area = 0.0;
    for (std::size_t index = 1; index < ring.size(); ++index)
        doubled_area += static_cast<long double>(ring[index - 1].x) * ring[index].y -
                        static_cast<long double>(ring[index].x) * ring[index - 1].y;
    return doubled_area / 2.0;
}

bool contains(const GeographicRing &ring, double x, double y)
{
    bool inside = false;
    if (ring.size() < 4) return false;
    for (std::size_t i = 0, j = ring.size() - 2; i + 1 < ring.size(); j = i++)
    {
        const MapCoordinate &a = ring[i];
        const MapCoordinate &b = ring[j];
        if ((a.y > y) != (b.y > y) &&
            x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)
            inside = !inside;
    }
    return inside;
}

bool containsGridRing(const std::vector<GridVertex> &ring, double x, double y)
{
    bool inside = false;
    for (std::size_t i = 0, j = ring.size() - 2; i + 1 < ring.size(); j = i++)
    {
        const GridVertex &a = ring[i];
        const GridVertex &b = ring[j];
        if ((a.y > y) != (b.y > y) &&
            x < (double(b.x) - a.x) * (y - a.y) / (double(b.y) - a.y) + a.x)
            inside = !inside;
    }
    return inside;
}

void addEdge(std::vector<Edge> &edges,
             std::unordered_map<std::uint64_t, Outgoing> &adjacency,
             std::uint64_t from, std::uint64_t to, std::uint8_t direction,
             std::uint64_t limit)
{
    if (edges.size() >= limit)
        throw std::invalid_argument("Too many catchment boundary edges; use a smaller analysis area");
    const std::size_t index = edges.size();
    edges.push_back({from, to, direction, false});
    const std::pair<std::unordered_map<std::uint64_t, Outgoing>::iterator, bool> entry =
        adjacency.try_emplace(from, Outgoing{-1, -1, -1, -1});
    if (entry.first->second[direction] != -1)
        throw std::runtime_error("Duplicate raster boundary edge");
    entry.first->second[direction] = std::int64_t(index);
}

// Drop collinear cell-edge vertices, but retain the explicit closure.
void simplifyRing(std::vector<GridVertex> *ring)
{
    const std::size_t count = ring->size() - 1;
    if (count <= 4) return;
    std::vector<GridVertex> simplified;
    for (std::size_t index = 0; index < count; ++index)
    {
        const GridVertex &previous = (*ring)[(index + count - 1) % count];
        const GridVertex &current = (*ring)[index];
        const GridVertex &next = (*ring)[(index + 1) % count];
        const std::int64_t dx1 = std::int64_t(current.x) - previous.x;
        const std::int64_t dy1 = std::int64_t(current.y) - previous.y;
        const std::int64_t dx2 = std::int64_t(next.x) - current.x;
        const std::int64_t dy2 = std::int64_t(next.y) - current.y;
        if (dx1 * dy2 != dy1 * dx2)
            simplified.push_back(current);
    }
    if (simplified.size() < 4)
        throw std::runtime_error("Degenerate catchment boundary ring");
    simplified.push_back(simplified.front());
    *ring = std::move(simplified);
}

bool touchesBoundary(std::uint64_t index, std::uint32_t columns, std::uint32_t rows)
{
    return index % columns == 0 || index % columns + 1 == columns ||
        index / columns == 0 || index / columns + 1 == rows;
}

bool windowContains(const DrainageWindow &window, const MapCoordinate &point)
{
    return point.x >= window.west && point.x <= window.east &&
        point.y >= window.south && point.y <= window.north;
}
}

MapCoordinate gridVertexWgs84(const RasterGeometry &geometry, const GridVertex &vertex)
{
    const MapCoordinate point = {
        geometry.origin_x + double(vertex.x) * geometry.pixel_width,
        geometry.origin_y - double(vertex.y) * geometry.pixel_height};
    if (geometry.horizontal_crs == HorizontalCrs::Wgs84Geographic)
        return point;
    return utmToWgs84({geometry.utm_zone, geometry.northern_hemisphere,
                       point.x, point.y});
}

WatershedBoundary polygonizeCatchment(const ElevationGrid &grid,
    const std::vector<std::uint8_t> &mask, std::uint64_t maximum_boundary_edges)
{
    validateRasterGeometry(grid.geometry, grid.columns, grid.rows);
    const std::uint64_t count = std::uint64_t(grid.columns) * grid.rows;
    if (mask.size() != count || maximum_boundary_edges < 4)
        throw std::invalid_argument("Invalid catchment mask or boundary budget");
    WatershedBoundary boundary;
    std::vector<Edge> edges;
    std::unordered_map<std::uint64_t, Outgoing> outgoing;
    for (std::uint32_t y = 0; y < grid.rows; ++y)
    {
        const double cell_area = rasterRowCellAreaM2(grid.geometry, grid.columns, y);
        for (std::uint32_t x = 0; x < grid.columns; ++x)
        {
            const std::uint64_t cell = std::uint64_t(y) * grid.columns + x;
            if (!mask[std::size_t(cell)]) continue;
            ++boundary.contributing_cells;
            boundary.contributing_area_m2 += cell_area;
            boundary.touches_domain_boundary |= touchesBoundary(cell, grid.columns, grid.rows);
            const std::uint64_t top_left = vertexKey(x, y, grid.columns);
            const std::uint64_t top_right = vertexKey(x + 1, y, grid.columns);
            const std::uint64_t bottom_left = vertexKey(x, y + 1, grid.columns);
            const std::uint64_t bottom_right = vertexKey(x + 1, y + 1, grid.columns);
            if (y == 0 || mask[std::size_t(cell - grid.columns)] == 0)
                addEdge(edges, outgoing, top_left, top_right, 0, maximum_boundary_edges);
            if (x + 1 == grid.columns || !mask[std::size_t(cell + 1)])
                addEdge(edges, outgoing, top_right, bottom_right, 1, maximum_boundary_edges);
            if (y + 1 == grid.rows || !mask[std::size_t(cell + grid.columns)])
                addEdge(edges, outgoing, bottom_right, bottom_left, 2, maximum_boundary_edges);
            if (x == 0 || !mask[std::size_t(cell - 1)])
                addEdge(edges, outgoing, bottom_left, top_left, 3, maximum_boundary_edges);
        }
    }
    if (boundary.contributing_cells == 0)
        throw std::invalid_argument("Catchment mask is empty");
    struct RingRecord { long double area = 0.0; std::vector<GridVertex> vertices; };
    std::vector<RingRecord> outer;
    std::vector<RingRecord> holes;
    for (std::size_t start = 0; start < edges.size(); ++start)
    {
        if (edges[start].consumed) continue;
        std::vector<GridVertex> ring;
        std::uint64_t current = start;
        bool closed = false;
        for (std::size_t step = 0; step < edges.size(); ++step)
        {
            Edge &edge = edges[std::size_t(current)];
            if (edge.consumed)
                throw std::runtime_error("Self-intersecting catchment boundary");
            edge.consumed = true;
            ring.push_back(decodeVertex(edge.from, grid.columns));
            if (edge.to == edges[start].from)
            {
                ring.push_back(ring.front());
                closed = true;
                break;
            }
            const std::unordered_map<std::uint64_t, Outgoing>::const_iterator next = outgoing.find(edge.to);
            if (next == outgoing.end())
                throw std::runtime_error("Open catchment boundary");
            std::int64_t following = -1;
            for (int turn : {1, 0, 3, 2})
            {
                const std::int64_t candidate = next->second[(edge.direction + turn) % 4];
                if (candidate >= 0 && !edges[std::size_t(candidate)].consumed)
                {
                    following = candidate;
                    break;
                }
            }
            if (following < 0)
                throw std::runtime_error("Incomplete catchment boundary ring");
            current = std::uint64_t(following);
        }
        if (!closed) throw std::runtime_error("Catchment boundary did not close");
        simplifyRing(&ring);
        const long double area = signedArea(ring);
        if (area == 0) throw std::runtime_error("Degenerate catchment boundary area");
        (area > 0 ? outer : holes).push_back({area, std::move(ring)});
    }
    for (RingRecord &ring : outer)
        boundary.polygons.push_back({{std::move(ring.vertices)}});
    for (RingRecord &hole : holes)
    {
        // Locate a point just inside the empty side of the first hole edge.
        const GridVertex &first = hole.vertices[0];
        const GridVertex &second = hole.vertices[1];
        const double dx = double(second.x) - first.x;
        const double dy = double(second.y) - first.y;
        const double length = std::hypot(dx, dy);
        const double px = (first.x + second.x) * 0.5 + 0.01 * dy / length;
        const double py = (first.y + second.y) * 0.5 - 0.01 * dx / length;
        std::size_t containing = boundary.polygons.size();
        long double best = std::numeric_limits<long double>::infinity();
        for (std::size_t index = 0; index < boundary.polygons.size(); ++index)
        {
            const std::vector<GridVertex> &outside = boundary.polygons[index].rings.front();
            const long double area = std::abs(signedArea(outside));
            if (area < best && containsGridRing(outside, px, py))
            {
                containing = index;
                best = area;
            }
        }
        if (containing >= boundary.polygons.size())
            throw std::runtime_error("Catchment hole has no enclosing outer ring");
        boundary.polygons[containing].rings.push_back(std::move(hole.vertices));
    }
    return boundary;
}

namespace
{
void validateWindow(const DrainageWindow &window, const char *message)
{
    if (!std::isfinite(window.west) || !std::isfinite(window.east) ||
        !std::isfinite(window.south) || !std::isfinite(window.north) ||
        window.west < -180.0 || window.east > 180.0 ||
        window.south < -90.0 || window.north > 90.0 ||
        window.west >= window.east || window.south >= window.north)
        throw std::invalid_argument(message);
}

bool insideAnyPolygon(const std::vector<GeographicPolygon> &polygons,
                      const MapCoordinate &coordinate)
{
    for (const GeographicPolygon &polygon : polygons)
    {
        if (!contains(polygon.exterior, coordinate.x, coordinate.y)) continue;
        bool in_hole = false;
        for (const GeographicRing &hole : polygon.holes)
            if (contains(hole, coordinate.x, coordinate.y)) { in_hole = true; break; }
        if (!in_hole) return true;
    }
    return false;
}
}

ChannelNetwork extractWindowChannelNetwork(
    const RasterGeometry &geometry, std::uint32_t columns, std::uint32_t rows,
    const DownstreamReader &downstream, const UpstreamAreaReader &upstream_area,
    double minimum_upstream_area_m2, const DrainageWindow &window,
    std::uint64_t maximum_selected_channel_cells, std::uint32_t maximum_segments)
{
    validateRasterGeometry(geometry, columns, rows);
    if (!downstream || !upstream_area)
        throw std::invalid_argument("Invalid drainage network spatial window");
    validateWindow(window, "Invalid drainage network spatial window");
    if (!std::isfinite(minimum_upstream_area_m2) || minimum_upstream_area_m2 <= 0.0 ||
        maximum_selected_channel_cells == 0 || maximum_segments == 0)
        throw std::invalid_argument("Invalid channel-network extraction limits");
    const std::uint64_t count = std::uint64_t(columns) * rows;
    std::vector<std::uint64_t> cells;
    std::vector<double> areas;
    for (std::uint64_t cell = 0; cell < count; ++cell)
    {
        const double area = upstream_area(cell);
        if (!(area >= minimum_upstream_area_m2)) continue;
        const MapCoordinate coordinate = pixelCentreWgs84(geometry,
            std::uint32_t(cell % columns), std::uint32_t(cell / columns));
        if (!windowContains(window, coordinate)) continue;
        if (cells.size() >= maximum_selected_channel_cells)
            throw std::invalid_argument("Too many selected channel cells; raise the accumulation threshold");
        cells.push_back(cell);
        areas.push_back(area);
    }
    return channelNetworkFromCells(geometry, columns, rows, cells, areas, downstream,
                                   minimum_upstream_area_m2, maximum_segments);
}

ChannelNetwork extractWindowChannelNetwork(
    const AnalysisResult &result, double minimum_upstream_area_m2,
    const DrainageWindow &window, std::uint64_t maximum_selected_channel_cells,
    std::uint32_t maximum_segments)
{
    validateRasterGeometry(result.source.geometry, result.source.columns, result.source.rows);
    const std::uint64_t count = std::uint64_t(result.source.columns) * result.source.rows;
    if (result.downstream.size() != count || result.upstream_area_m2.size() != count)
        throw std::invalid_argument("Invalid drainage network spatial window");
    return extractWindowChannelNetwork(result.source.geometry, result.source.columns,
        result.source.rows,
        [&result](std::uint64_t i) { return result.downstream[std::size_t(i)]; },
        [&result](std::uint64_t i) { return result.upstream_area_m2[std::size_t(i)]; },
        minimum_upstream_area_m2, window, maximum_selected_channel_cells, maximum_segments);
}

PolygonDrainageResult downstreamNetworkFromPolygon(
    const RasterGeometry &geometry, std::uint32_t columns, std::uint32_t rows,
    const DownstreamReader &downstream, const UpstreamAreaReader &upstream_area,
    const std::vector<GeographicPolygon> &polygons,
    double minimum_upstream_area_m2, const DrainageWindow &window,
    std::uint64_t maximum_source_cells, std::uint64_t maximum_affected_cells,
    std::uint64_t maximum_selected_channel_cells, std::uint32_t maximum_segments)
{
    validateRasterGeometry(geometry, columns, rows);
    const std::uint64_t count = std::uint64_t(columns) * rows;
    if (!downstream || !upstream_area ||
        polygons.empty() || polygons.size() > 32 ||
        !std::isfinite(minimum_upstream_area_m2) || minimum_upstream_area_m2 <= 0.0 ||
        maximum_source_cells == 0 || maximum_affected_cells == 0)
        throw std::invalid_argument("Invalid downstream polygon query parameters");
    validateWindow(window, "Invalid downstream polygon query parameters");
    double west = 180.0, east = -180.0, south = 90.0, north = -90.0;
    for (const GeographicPolygon &polygon : polygons)
    {
        if (polygon.exterior.size() < 4 || polygon.exterior.size() > 2048 ||
            polygon.holes.size() > 64)
            throw std::invalid_argument("Invalid polygon ring count");
        const std::vector<GeographicRing> rings = [&polygon]()
        {
            std::vector<GeographicRing> value{polygon.exterior};
            value.insert(value.end(), polygon.holes.begin(), polygon.holes.end());
            return value;
        }();
        for (const GeographicRing &ring : rings)
        {
            if (ring.size() < 4 || ring.size() > 2048 ||
                ring.front().x != ring.back().x || ring.front().y != ring.back().y)
                throw std::invalid_argument("Invalid or unclosed polygon ring");
            for (const MapCoordinate &point : ring)
                if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                    point.x < -180.0 || point.x > 180.0 || point.y < -90.0 || point.y > 90.0)
                    throw std::invalid_argument("Invalid WGS84 polygon vertex");
        }
        for (const MapCoordinate &point : polygon.exterior)
        {
            west = std::min(west, point.x);
            east = std::max(east, point.x);
            south = std::min(south, point.y);
            north = std::max(north, point.y);
        }
    }
    // Cell centres inside a polygon lie inside its exterior bounding box. For
    // geographic grids that box maps directly to a pixel range, so only those
    // cells are tested and read; projected grids scan every cell.
    std::uint32_t x0 = 0, x1 = columns, y0 = 0, y1 = rows;
    if (geometry.horizontal_crs == HorizontalCrs::Wgs84Geographic)
    {
        const auto clampIndex = [](double value, std::uint32_t limit)
        {
            return std::uint32_t(std::clamp(value, 0.0, double(limit)));
        };
        x0 = clampIndex(std::floor((west - geometry.origin_x) / geometry.pixel_width - 0.5) - 1.0, columns);
        x1 = clampIndex(std::ceil((east - geometry.origin_x) / geometry.pixel_width - 0.5) + 2.0, columns);
        y0 = clampIndex(std::floor((geometry.origin_y - north) / geometry.pixel_height - 0.5) - 1.0, rows);
        y1 = clampIndex(std::ceil((geometry.origin_y - south) / geometry.pixel_height - 0.5) + 2.0, rows);
    }
    PolygonDrainageResult output;
    std::vector<std::uint64_t> sources;
    for (std::uint32_t y = y0; y < y1; ++y)
        for (std::uint32_t x = x0; x < x1; ++x)
        {
            const std::uint64_t cell = std::uint64_t(y) * columns + x;
            if (upstream_area(cell) <= 0.0) continue;
            if (!insideAnyPolygon(polygons, pixelCentreWgs84(geometry, x, y))) continue;
            sources.push_back(cell);
            if (++output.source_cells > maximum_source_cells)
                throw std::invalid_argument("Too many rainfall source cells in polygon query");
        }
    if (output.source_cells == 0)
        throw std::invalid_argument("Polygon contains no valid analysis cell centres");
    // Sources are ascending (row-major scan), exactly the order of a full scan.
    std::unordered_map<std::uint64_t, std::uint8_t> affected;
    std::unordered_map<std::uint64_t, std::uint32_t> walk_generation;
    std::vector<std::uint64_t> path;
    std::uint32_t generation = 0;
    for (const std::uint64_t source : sources)
    {
        if (affected.count(source)) continue;
        if (++generation == 0)
            throw std::runtime_error("Too many polygon trace traversals");
        path.clear();
        std::uint64_t current = source;
        while (!affected.count(current))
        {
            const std::unordered_map<std::uint64_t, std::uint32_t>::iterator seen =
                walk_generation.find(current);
            if (seen != walk_generation.end() && seen->second == generation)
                throw std::runtime_error("Flow graph has a cycle");
            walk_generation[current] = generation;
            path.push_back(current);
            if (output.downstream_affected_cells + path.size() > maximum_affected_cells)
                throw std::invalid_argument("Too many downstream affected cells for polygon query");
            const std::int64_t next = downstream(current);
            if (next == -1) break;
            if (next < 0 || std::uint64_t(next) >= count ||
                std::abs(std::int64_t(current % columns) - (next % columns)) > 1 ||
                std::abs(std::int64_t(current / columns) - (next / columns)) > 1 ||
                next == std::int64_t(current))
                throw std::runtime_error("Invalid downstream link in polygon query");
            current = std::uint64_t(next);
        }
        for (const std::uint64_t cell : path)
        {
            affected.emplace(cell, 1);
            ++output.downstream_affected_cells;
            if (touchesBoundary(cell, columns, rows))
                ++output.affected_boundary_cells;
        }
    }
    // Locally selected channel network using true FULL-DOMAIN upstream areas,
    // but only links within the affected/visible subset.
    std::vector<std::uint64_t> ordered;
    ordered.reserve(affected.size());
    for (const std::pair<const std::uint64_t, std::uint8_t> &entry : affected)
        ordered.push_back(entry.first);
    std::sort(ordered.begin(), ordered.end());
    if (maximum_selected_channel_cells == 0 || maximum_segments == 0)
        throw std::invalid_argument("Invalid channel-network extraction limits");
    std::vector<std::uint64_t> cells;
    std::vector<double> areas;
    for (const std::uint64_t cell : ordered)
    {
        const MapCoordinate coordinate = pixelCentreWgs84(geometry,
            std::uint32_t(cell % columns), std::uint32_t(cell / columns));
        if (!windowContains(window, coordinate))
        {
            output.clipped_to_window = true;
            continue;
        }
        const double area = upstream_area(cell);
        if (!(area >= minimum_upstream_area_m2)) continue;
        if (cells.size() >= maximum_selected_channel_cells)
            throw std::invalid_argument("Too many selected channel cells; raise the accumulation threshold");
        cells.push_back(cell);
        areas.push_back(area);
    }
    output.network = channelNetworkFromCells(geometry, columns, rows, cells, areas,
        downstream, minimum_upstream_area_m2, maximum_segments);
    return output;
}

PolygonDrainageResult downstreamNetworkFromPolygon(
    const AnalysisResult &result, const std::vector<GeographicPolygon> &polygons,
    double minimum_upstream_area_m2, const DrainageWindow &window,
    std::uint64_t maximum_source_cells, std::uint64_t maximum_affected_cells,
    std::uint64_t maximum_selected_channel_cells, std::uint32_t maximum_segments)
{
    validateRasterGeometry(result.source.geometry, result.source.columns, result.source.rows);
    const std::uint64_t count = std::uint64_t(result.source.columns) * result.source.rows;
    if (result.downstream.size() != count || result.upstream_area_m2.size() != count)
        throw std::invalid_argument("Invalid downstream polygon query parameters");
    return downstreamNetworkFromPolygon(result.source.geometry, result.source.columns,
        result.source.rows,
        [&result](std::uint64_t i) { return result.downstream[std::size_t(i)]; },
        [&result](std::uint64_t i) { return result.upstream_area_m2[std::size_t(i)]; },
        polygons, minimum_upstream_area_m2, window, maximum_source_cells,
        maximum_affected_cells, maximum_selected_channel_cells, maximum_segments);
}

} // namespace Aowis::Hydrology
