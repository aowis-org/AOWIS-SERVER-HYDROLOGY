#include <aowis/hydrology/flow_network.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace Aowis::Hydrology
{
namespace
{
void checkNext(std::uint64_t source, std::int64_t downstream,
               std::uint32_t columns, std::uint32_t rows)
{
    if (downstream == -1) return;
    const std::uint64_t count = std::uint64_t(columns) * rows;
    if (downstream < 0 || std::uint64_t(downstream) >= count ||
        downstream == std::int64_t(source))
        throw std::runtime_error("Invalid downstream cell index in flow graph");
    const std::int64_t x = std::int64_t(source % columns);
    const std::int64_t y = std::int64_t(source / columns);
    const std::int64_t next_x = downstream % columns;
    const std::int64_t next_y = downstream / columns;
    if (std::abs(next_x - x) > 1 || std::abs(next_y - y) > 1)
        throw std::runtime_error("Flow graph contains a nonadjacent downstream link");
}

double lengthM(const RasterGeometry &geometry, std::uint32_t columns,
               std::uint64_t from, std::uint64_t to)
{
    const std::int64_t x = std::int64_t(to % columns) - std::int64_t(from % columns);
    const std::int64_t y = std::int64_t(to / columns) - std::int64_t(from / columns);
    return rasterRowNeighborDistanceM(geometry, columns,
        std::uint32_t(from / columns), int(x), int(y));
}

bool boundary(std::uint64_t index, std::uint32_t columns, std::uint32_t rows)
{
    return index % columns == 0 || index % columns + 1 == columns ||
        index / columns == 0 || index / columns + 1 == rows;
}
}

FlowTrace traceDownstream(const RasterGeometry &geometry,
                          std::uint32_t columns, std::uint32_t rows,
                          std::uint64_t start_cell, double maximum_distance_m,
                          std::uint32_t maximum_points,
                          const DownstreamReader &downstream,
                          const ElevationReader &elevation)
{
    validateRasterGeometry(geometry, columns, rows);
    const std::uint64_t count = std::uint64_t(columns) * rows;
    if (start_cell >= count || !std::isfinite(maximum_distance_m) ||
        maximum_distance_m <= 0.0 || maximum_points < 1 || maximum_points > 100000 ||
        !downstream || !elevation)
        throw std::invalid_argument("Invalid downstream tracing parameters");
    const float first_height = elevation(start_cell);
    if (!std::isfinite(first_height))
        throw std::invalid_argument("Downstream tracing starts at a missing elevation cell");
    FlowTrace trace;
    trace.points.push_back({start_cell, 0.0, first_height});
    std::uint64_t cell = start_cell;
    for (std::uint64_t visited = 0; visited < count; ++visited)
    {
        const std::int64_t next = downstream(cell);
        checkNext(cell, next, columns, rows);
        if (next == -1)
        {
            trace.termination = boundary(cell, columns, rows)
                ? TraceTermination::DomainOutlet : TraceTermination::InternalSink;
            break;
        }
        if (trace.points.size() >= maximum_points)
        {
            trace.termination = TraceTermination::PointLimit;
            break;
        }
        const double increment = lengthM(geometry, columns, cell, std::uint64_t(next));
        if (!std::isfinite(increment) || increment <= 0.0)
            throw std::runtime_error("Invalid physical distance along flow link");
        if (trace.traced_length_m + increment > maximum_distance_m)
        {
            trace.termination = TraceTermination::DistanceLimit;
            break;
        }
        const float height = elevation(std::uint64_t(next));
        if (!std::isfinite(height))
            throw std::runtime_error("Flow graph leads into missing terrain");
        trace.traced_length_m += increment;
        trace.points.push_back({std::uint64_t(next), trace.traced_length_m, height});
        cell = std::uint64_t(next);
        if (visited + 1 == count)
            throw std::runtime_error("Flow graph contains a cycle");
    }
    if (trace.traced_length_m > 0.0)
        trace.net_slope = double(first_height - trace.points.back().conditioned_elevation_m)
            / trace.traced_length_m;
    return trace;
}

FlowTrace traceDownstream(const AnalysisResult &result,
                          std::uint64_t start_cell, double maximum_distance_m,
                          std::uint32_t maximum_points)
{
    const std::uint64_t count = std::uint64_t(result.source.columns) * result.source.rows;
    if (result.downstream.size() != count || result.conditioned_elevations_m.size() != count)
        throw std::invalid_argument("Incomplete flow graph for downstream tracing");
    return traceDownstream(result.source.geometry, result.source.columns, result.source.rows,
        start_cell, maximum_distance_m, maximum_points,
        [&result](std::uint64_t i) { return result.downstream[std::size_t(i)]; },
        [&result](std::uint64_t i) { return result.conditioned_elevations_m[std::size_t(i)]; });
}

ChannelNetwork channelNetworkFromCells(const RasterGeometry &geometry,
                                       std::uint32_t columns, std::uint32_t rows,
                                       const std::vector<std::uint64_t> &selected_cells,
                                       const std::vector<double> &selected_areas_m2,
                                       const DownstreamReader &downstream,
                                       double threshold_m2,
                                       std::uint32_t maximum_segments)
{
    validateRasterGeometry(geometry, columns, rows);
    const std::uint64_t count = std::uint64_t(columns) * rows;
    if (selected_cells.size() != selected_areas_m2.size() || !downstream ||
        maximum_segments == 0)
        throw std::invalid_argument("Invalid channel-network cell selection");
    for (std::size_t i = 0; i < selected_cells.size(); ++i)
        if (selected_cells[i] >= count || (i > 0 && selected_cells[i] <= selected_cells[i - 1]))
            throw std::invalid_argument("Channel-network selection must be ascending grid cells");
    const std::size_t n = selected_cells.size();
    ChannelNetwork network;
    network.threshold_m2 = threshold_m2;
    network.selected_cells = n;
    std::unordered_map<std::uint64_t, std::size_t> position;
    position.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        position.emplace(selected_cells[i], i);
    // Downstream cell and its position inside the selection (-1 if outside).
    std::vector<std::int64_t> next_cell(n, -1);
    std::vector<std::int64_t> next_local(n, -1);
    std::vector<std::uint8_t> incoming(n, 0);
    for (std::size_t i = 0; i < n; ++i)
    {
        const std::int64_t next = downstream(selected_cells[i]);
        checkNext(selected_cells[i], next, columns, rows);
        next_cell[i] = next;
        if (next < 0) continue;
        const std::unordered_map<std::uint64_t, std::size_t>::const_iterator found =
            position.find(std::uint64_t(next));
        if (found == position.cend()) continue;
        next_local[i] = std::int64_t(found->second);
        ++incoming[found->second];
    }
    std::vector<std::uint8_t> remaining = incoming;
    std::vector<std::uint16_t> order(n, 0);
    std::vector<std::uint16_t> max_order(n, 0);
    std::vector<std::uint8_t> max_count(n, 0);
    std::queue<std::size_t> ready;
    for (std::size_t i = 0; i < n; ++i)
        if (incoming[i] == 0) ready.push(i);
    std::uint64_t processed = 0;
    while (!ready.empty())
    {
        const std::size_t cell = ready.front();
        ready.pop();
        ++processed;
        const std::uint16_t best = max_order[cell];
        order[cell] = best == 0 ? 1 : std::uint16_t(best + (max_count[cell] >= 2 ? 1 : 0));
        const std::int64_t next = next_local[cell];
        if (next >= 0)
        {
            const std::size_t target = std::size_t(next);
            if (order[cell] > max_order[target])
            {
                max_order[target] = order[cell];
                max_count[target] = 1;
            }
            else if (order[cell] == max_order[target])
                ++max_count[target];
            if (--remaining[target] == 0)
                ready.push(target);
        }
    }
    if (processed != n)
        throw std::runtime_error("Channel graph contains a cycle");

    std::vector<std::uint8_t> node(n, 0);
    std::vector<std::size_t> node_positions;
    for (std::size_t i = 0; i < n; ++i)
    {
        if (incoming[i] != 1 || next_local[i] < 0)
        {
            node[i] = 1;
            node_positions.push_back(i);
            network.nodes.push_back({selected_cells[i], incoming[i], order[i]});
        }
    }
    for (const std::size_t start : node_positions)
    {
        if (next_local[start] < 0) continue;
        if (network.segments.size() >= maximum_segments)
            throw std::invalid_argument("Too many channel segments; raise the accumulation threshold");
        ChannelSegment segment;
        segment.id = selected_cells[start];
        segment.from_cell = selected_cells[start];
        segment.strahler_order = order[start];
        segment.cells.push_back(selected_cells[start]);
        std::size_t current = start;
        for (std::uint64_t traversed = 0; traversed < n; ++traversed)
        {
            const std::int64_t following = next_local[current];
            if (following < 0)
                throw std::runtime_error("Channel segment terminates outside selected network");
            segment.length_m += lengthM(geometry, columns, selected_cells[current],
                                        std::uint64_t(next_cell[current]));
            current = std::size_t(following);
            segment.cells.push_back(selected_cells[current]);
            if (node[current]) break;
            if (traversed + 1 == n)
                throw std::runtime_error("Channel segment has no downstream junction or outlet");
        }
        segment.to_cell = selected_cells[current];
        segment.upstream_area_at_end_m2 = selected_areas_m2[current];
        network.segments.push_back(std::move(segment));
    }
    return network;
}

ChannelNetwork extractChannelNetwork(const RasterGeometry &geometry,
                                     std::uint32_t columns, std::uint32_t rows,
                                     const DownstreamReader &downstream,
                                     const UpstreamAreaReader &upstream_area,
                                     double minimum_upstream_area_m2,
                                     std::uint64_t maximum_selected_cells,
                                     std::uint32_t maximum_segments)
{
    validateRasterGeometry(geometry, columns, rows);
    if (!downstream || !upstream_area)
        throw std::invalid_argument("Incomplete flow graph for channel extraction");
    if (!std::isfinite(minimum_upstream_area_m2) || minimum_upstream_area_m2 <= 0.0 ||
        maximum_selected_cells == 0 || maximum_segments == 0)
        throw std::invalid_argument("Invalid channel-network extraction limits");
    const std::uint64_t count = std::uint64_t(columns) * rows;
    std::vector<std::uint64_t> cells;
    std::vector<double> areas;
    for (std::uint64_t index = 0; index < count; ++index)
    {
        const double area = upstream_area(index);
        if (!(area >= minimum_upstream_area_m2)) continue;
        if (cells.size() >= maximum_selected_cells)
            throw std::invalid_argument("Too many selected channel cells; raise the accumulation threshold");
        cells.push_back(index);
        areas.push_back(area);
    }
    return channelNetworkFromCells(geometry, columns, rows, cells, areas, downstream,
                                   minimum_upstream_area_m2, maximum_segments);
}

ChannelNetwork extractChannelNetwork(const AnalysisResult &result,
                                     double minimum_upstream_area_m2,
                                     std::uint64_t maximum_selected_cells,
                                     std::uint32_t maximum_segments)
{
    const std::uint64_t count = std::uint64_t(result.source.columns) * result.source.rows;
    validateRasterGeometry(result.source.geometry, result.source.columns, result.source.rows);
    if (result.downstream.size() != count || result.upstream_area_m2.size() != count)
        throw std::invalid_argument("Incomplete flow graph for channel extraction");
    return extractChannelNetwork(result.source.geometry, result.source.columns,
        result.source.rows,
        [&result](std::uint64_t i) { return result.downstream[std::size_t(i)]; },
        [&result](std::uint64_t i) { return result.upstream_area_m2[std::size_t(i)]; },
        minimum_upstream_area_m2, maximum_selected_cells, maximum_segments);
}

} // namespace Aowis::Hydrology
