#include <aowis/hydrology/flow_network.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
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

ChannelNetwork extractChannelNetwork(const AnalysisResult &result,
                                     double minimum_upstream_area_m2,
                                     std::uint64_t maximum_selected_cells,
                                     std::uint32_t maximum_segments)
{
    const std::uint32_t columns = result.source.columns;
    const std::uint32_t rows = result.source.rows;
    validateRasterGeometry(result.source.geometry, columns, rows);
    const std::uint64_t count = std::uint64_t(columns) * rows;
    if (result.downstream.size() != count || result.upstream_area_m2.size() != count)
        throw std::invalid_argument("Incomplete flow graph for channel extraction");
    if (!std::isfinite(minimum_upstream_area_m2) || minimum_upstream_area_m2 <= 0.0 ||
        maximum_selected_cells == 0 || maximum_segments == 0)
        throw std::invalid_argument("Invalid channel-network extraction limits");
    ChannelNetwork network;
    network.threshold_m2 = minimum_upstream_area_m2;
    std::vector<std::uint8_t> selected(std::size_t(count), 0);
    for (std::uint64_t index = 0; index < count; ++index)
    {
        if (result.upstream_area_m2[std::size_t(index)] >= minimum_upstream_area_m2)
        {
            selected[std::size_t(index)] = 1;
            ++network.selected_cells;
            if (network.selected_cells > maximum_selected_cells)
                throw std::invalid_argument("Too many selected channel cells; raise the accumulation threshold");
        }
    }
    std::vector<std::uint8_t> incoming(std::size_t(count), 0);
    for (std::uint64_t index = 0; index < count; ++index)
    {
        if (!selected[std::size_t(index)]) continue;
        const std::int64_t next = result.downstream[std::size_t(index)];
        checkNext(index, next, columns, rows);
        if (next >= 0 && selected[std::size_t(next)])
            ++incoming[std::size_t(next)];
    }
    std::vector<std::uint8_t> remaining = incoming;
    std::vector<std::uint16_t> order(std::size_t(count), 0);
    std::vector<std::uint16_t> max_order(std::size_t(count), 0);
    std::vector<std::uint8_t> max_count(std::size_t(count), 0);
    std::queue<std::uint64_t> ready;
    for (std::uint64_t index = 0; index < count; ++index)
        if (selected[std::size_t(index)] && incoming[std::size_t(index)] == 0)
            ready.push(index);
    std::uint64_t processed = 0;
    while (!ready.empty())
    {
        const std::uint64_t cell = ready.front();
        ready.pop();
        ++processed;
        const std::uint16_t best = max_order[std::size_t(cell)];
        order[std::size_t(cell)] = best == 0 ? 1 :
            std::uint16_t(best + (max_count[std::size_t(cell)] >= 2 ? 1 : 0));
        const std::int64_t next = result.downstream[std::size_t(cell)];
        if (next >= 0 && selected[std::size_t(next)])
        {
            if (order[std::size_t(cell)] > max_order[std::size_t(next)])
            {
                max_order[std::size_t(next)] = order[std::size_t(cell)];
                max_count[std::size_t(next)] = 1;
            }
            else if (order[std::size_t(cell)] == max_order[std::size_t(next)])
                ++max_count[std::size_t(next)];
            if (--remaining[std::size_t(next)] == 0)
                ready.push(std::uint64_t(next));
        }
    }
    if (processed != network.selected_cells)
        throw std::runtime_error("Channel graph contains a cycle");

    std::vector<std::uint8_t> node(std::size_t(count), 0);
    for (std::uint64_t index = 0; index < count; ++index)
    {
        if (!selected[std::size_t(index)]) continue;
        const std::int64_t next = result.downstream[std::size_t(index)];
        if (incoming[std::size_t(index)] != 1 || next < 0 ||
            !selected[std::size_t(next)])
        {
            node[std::size_t(index)] = 1;
            network.nodes.push_back({index, incoming[std::size_t(index)], order[std::size_t(index)]});
        }
    }
    for (const ChannelNode &start : network.nodes)
    {
        const std::int64_t next = result.downstream[std::size_t(start.cell_index)];
        if (next < 0 || !selected[std::size_t(next)]) continue;
        if (network.segments.size() >= maximum_segments)
            throw std::invalid_argument("Too many channel segments; raise the accumulation threshold");
        ChannelSegment segment;
        segment.id = start.cell_index;
        segment.from_cell = start.cell_index;
        segment.strahler_order = start.strahler_order;
        segment.cells.push_back(start.cell_index);
        std::uint64_t current = start.cell_index;
        for (std::uint64_t traversed = 0; traversed < network.selected_cells; ++traversed)
        {
            const std::int64_t next_cell = result.downstream[std::size_t(current)];
            if (next_cell < 0 || !selected[std::size_t(next_cell)])
                throw std::runtime_error("Channel segment terminates outside selected network");
            segment.length_m += lengthM(result.source.geometry, columns,
                                       current, std::uint64_t(next_cell));
            current = std::uint64_t(next_cell);
            segment.cells.push_back(current);
            if (node[std::size_t(current)]) break;
            if (traversed + 1 == network.selected_cells)
                throw std::runtime_error("Channel segment has no downstream junction or outlet");
        }
        segment.to_cell = current;
        segment.upstream_area_at_end_m2 = result.upstream_area_m2[std::size_t(current)];
        network.segments.push_back(std::move(segment));
    }
    return network;
}

} // namespace Aowis::Hydrology
