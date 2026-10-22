#include <aowis/hydrology/analysis.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>
#include <utility>

namespace Aowis::Hydrology
{
namespace
{
constexpr int NeighborDx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int NeighborDy[8] = {0, 1, 1, 1, 0, -1, -1, -1};

struct FloodCell
{
    float elevation_m;
    std::uint64_t index;
};

struct GreaterFloodCell
{
    bool operator()(const FloodCell &left, const FloodCell &right) const
    {
        if (left.elevation_m != right.elevation_m)
            return left.elevation_m > right.elevation_m;
        return left.index > right.index;
    }
};

bool validCell(const ElevationGrid &grid, std::uint64_t index)
{
    return std::isfinite(grid.elevations_m[std::size_t(index)]);
}

bool neighbor(const ElevationGrid &grid, std::uint64_t index, int direction,
              std::uint64_t *out_index)
{
    const std::int64_t x = std::int64_t(index % grid.columns) + NeighborDx[direction];
    const std::int64_t y = std::int64_t(index / grid.columns) + NeighborDy[direction];
    if (x < 0 || y < 0 || x >= grid.columns || y >= grid.rows)
        return false;
    *out_index = std::uint64_t(y) * grid.columns + std::uint64_t(x);
    return true;
}

void validateGrid(const ElevationGrid &grid)
{
    if (std::uint64_t(grid.columns) * grid.rows != grid.elevations_m.size())
        throw std::invalid_argument("Elevation array size differs from raster geometry");
    validateRasterGeometry(grid.geometry, grid.columns, grid.rows);
}

std::vector<float> priorityFlood(const ElevationGrid &grid,
                                 const std::vector<float> &source,
                                 bool allow_holes,
                                 const std::function<void()> &checkCancellation)
{
    const std::uint64_t count = std::uint64_t(grid.columns) * grid.rows;
    std::vector<float> filled = source;
    std::vector<std::uint8_t> visited(std::size_t(count), 0);
    std::priority_queue<FloodCell, std::vector<FloodCell>, GreaterFloodCell> frontier;
    for (std::uint64_t index = 0; index < count; ++index)
    {
        if ((index & 4095U) == 0) checkCancellation();
        if (!std::isfinite(source[std::size_t(index)]))
            continue;
        const std::uint64_t x = index % grid.columns;
        const std::uint64_t y = index / grid.columns;
        bool boundary = x == 0 || y == 0 || x + 1 == grid.columns || y + 1 == grid.rows;
        if (!boundary && allow_holes)
        {
            for (int direction = 0; direction < 8; ++direction)
            {
                std::uint64_t adjacent = 0;
                if (neighbor(grid, index, direction, &adjacent) &&
                    !std::isfinite(source[std::size_t(adjacent)]))
                    boundary = true;
            }
        }
        if (boundary)
        {
            visited[std::size_t(index)] = 1;
            frontier.push({filled[std::size_t(index)], index});
        }
    }
    std::uint64_t steps = 0;
    while (!frontier.empty())
    {
        if ((++steps & 4095U) == 0) checkCancellation();
        const FloodCell current = frontier.top();
        frontier.pop();
        for (int direction = 0; direction < 8; ++direction)
        {
            std::uint64_t adjacent = 0;
            if (!neighbor(grid, current.index, direction, &adjacent) ||
                visited[std::size_t(adjacent)] ||
                !std::isfinite(source[std::size_t(adjacent)]))
                continue;
            visited[std::size_t(adjacent)] = 1;
            const float level = std::max(source[std::size_t(adjacent)], current.elevation_m);
            filled[std::size_t(adjacent)] = level;
            frontier.push({level, adjacent});
        }
    }
    return filled;
}

struct BreachStep
{
    double cost_m3 = 0.0;
    std::uint64_t index = 0;
    std::uint32_t steps = 0;
};

struct GreaterBreachStep
{
    bool operator()(const BreachStep &left, const BreachStep &right) const
    {
        if (left.cost_m3 != right.cost_m3)
            return left.cost_m3 > right.cost_m3;
        if (left.steps != right.steps)
            return left.steps > right.steps;
        return left.index > right.index;
    }
};

// Deliberately bounded local, least-excavation search for closed pits. A route
// must finish on terrain lower than the pit that already drains without a
// further fill above the pit's elevation. All cuts stay at or above pit level,
// so no invented epsilon gradient affects physical storage. Flat propagation
// in the flow graph handles the resulting zero-gradient trench.
void carveConstrainedBreaches(const ElevationGrid &grid,
                             const std::vector<float> &original_fill,
                             const AnalysisOptions &options,
                             AnalysisResult &result,
                             const std::function<void()> &checkCancellation)
{
    AnalysisOptions preserve;
    preserve.fill_depressions = false;
    preserve.reject_interior_nodata = options.reject_interior_nodata;
    const AnalysisResult unfilled = analyze(grid, preserve);
    const std::uint32_t max_steps = options.maximum_breach_path_cells;
    const std::uint64_t count = std::uint64_t(grid.columns) * grid.rows;
    // Per-search state is localized to the configured maximum path radius,
    // not allocated once per whole region times the number of steps.
    const std::uint32_t side = 2 * max_steps + 1;
    const std::size_t stride = std::size_t(side) * side;
    std::vector<double> best(stride * (max_steps + 1),
                             std::numeric_limits<double>::infinity());
    std::vector<std::int64_t> parent(best.size(), -1);
    std::uint32_t candidates = 0;
    for (std::uint64_t sink = 0; sink < count; ++sink)
    {
        if ((sink & 4095U) == 0) checkCancellation();
        if (unfilled.downstream[std::size_t(sink)] >= 0 ||
            !std::isfinite(grid.elevations_m[std::size_t(sink)]) ||
            original_fill[std::size_t(sink)] <= grid.elevations_m[std::size_t(sink)])
            continue;
        if (++candidates > options.maximum_breach_candidates)
            throw std::runtime_error("Constrained breach candidate limit exceeded; increase limit or use fill/preserve");
        const double sink_height = grid.elevations_m[std::size_t(sink)];
        const std::int64_t sink_x = std::int64_t(sink % grid.columns);
        const std::int64_t sink_y = std::int64_t(sink / grid.columns);
        std::fill(best.begin(), best.end(), std::numeric_limits<double>::infinity());
        std::fill(parent.begin(), parent.end(), -1);
        const std::function<std::size_t(std::uint64_t, std::uint32_t)> stateIndex =
            [&](std::uint64_t index, std::uint32_t steps)
        {
            const std::int64_t x = std::int64_t(index % grid.columns) - sink_x + max_steps;
            const std::int64_t y = std::int64_t(index / grid.columns) - sink_y + max_steps;
            return std::size_t(steps) * stride + std::size_t(y) * side + std::size_t(x);
        };
        best[stateIndex(sink, 0)] = 0.0;
        std::priority_queue<BreachStep, std::vector<BreachStep>, GreaterBreachStep> pending;
        pending.push({0.0, sink, 0});
        std::size_t exit_state = best.size();
        std::uint64_t iterations = 0;
        while (!pending.empty())
        {
            if ((++iterations & 4095U) == 0) checkCancellation();
            const BreachStep current = pending.top();
            pending.pop();
            const std::size_t current_state = stateIndex(current.index, current.steps);
            if (current.cost_m3 != best[current_state])
                continue;
            // Require an actual lower, boundary-connected outflow. Starting
            // cell cannot qualify; a zero-excavation path is not a breach.
            if (current.steps > 0 &&
                grid.elevations_m[std::size_t(current.index)] < sink_height &&
                original_fill[std::size_t(current.index)] <= sink_height)
            {
                exit_state = current_state;
                break;
            }
            if (current.steps == max_steps)
                continue;
            for (int direction = 0; direction < 8; ++direction)
            {
                std::uint64_t next = 0;
                if (!neighbor(grid, current.index, direction, &next) || !validCell(grid, next))
                    continue;
                if (std::abs(std::int64_t(next % grid.columns) - sink_x) > max_steps ||
                    std::abs(std::int64_t(next / grid.columns) - sink_y) > max_steps)
                    continue;
                const double height = result.conditioned_elevations_m[std::size_t(next)];
                const double cut = std::max(0.0, height - sink_height);
                // Enforce maximum *cumulative* cut depth from the immutable
                // source DEM, including previously accepted local trenches.
                const double total_cut = std::max(0.0,
                    double(grid.elevations_m[std::size_t(next)]) - sink_height);
                if (total_cut > options.maximum_breach_cut_depth_m)
                    continue;
                const double area = rasterRowCellAreaM2(grid.geometry, grid.columns,
                    std::uint32_t(next / grid.columns));
                // Tiny preference for shorter paths among equal excavation
                // volumes. It is too small to affect metre-scale cut choices.
                const double cost = current.cost_m3 + cut * area + 0.000001;
                if (cost > options.maximum_breach_excavation_m3)
                    continue;
                const std::size_t next_state = stateIndex(next, current.steps + 1);
                if (cost >= best[next_state])
                    continue;
                best[next_state] = cost;
                parent[next_state] = std::int64_t(current_state);
                pending.push({cost, next, current.steps + 1});
            }
        }
        if (exit_state == best.size())
        {
            ++result.rejected_breach_candidates;
            continue;
        }
        // Backtrack to collect path cells; stateIndex encodes their local
        // row/column and hop count. Only accept a path with actual cuts.
        std::vector<std::uint64_t> path;
        std::size_t state = exit_state;
        while (parent[state] >= 0)
        {
            const std::size_t tile = state % stride;
            const std::int64_t x = sink_x + std::int64_t(tile % side) - max_steps;
            const std::int64_t y = sink_y + std::int64_t(tile / side) - max_steps;
            path.push_back(std::uint64_t(y) * grid.columns + std::uint64_t(x));
            state = std::size_t(parent[state]);
        }
        bool modified = false;
        for (std::uint64_t index : path)
        {
            const float original = result.conditioned_elevations_m[std::size_t(index)];
            if (original <= sink_height)
                continue;
            result.conditioned_elevations_m[std::size_t(index)] = float(sink_height);
            modified = true;
        }
        if (modified)
            ++result.accepted_breach_paths;
        else
            ++result.rejected_breach_candidates;
    }
}

}

AnalysisResult analyze(const ElevationGrid &grid, const AnalysisOptions &options,
                       const std::atomic<bool> *cancel_requested)
{
    const std::function<void()> checkCancellation = [cancel_requested]()
    {
        if (cancel_requested != nullptr && cancel_requested->load(std::memory_order_relaxed))
            throw std::runtime_error("Job canceled");
    };
    checkCancellation();
    validateGrid(grid);
    const std::uint64_t count = std::uint64_t(grid.columns) * grid.rows;
    AnalysisResult result;
    result.source = grid;
    result.conditioned_elevations_m = grid.elevations_m;
    result.downstream.assign(std::size_t(count), -1);
    result.upstream_cell_count.assign(std::size_t(count), 0);
    result.upstream_area_m2.assign(std::size_t(count), 0.0);

    if (options.reject_interior_nodata)
    {
        for (std::uint32_t row = 1; row + 1 < grid.rows; ++row)
        {
            if ((row & 63U) == 0) checkCancellation();
            for (std::uint32_t column = 1; column + 1 < grid.columns; ++column)
            {
                const std::uint64_t index = std::uint64_t(row) * grid.columns + column;
                if (!validCell(grid, index))
                    throw std::invalid_argument("Missing interior DEM cell; fix source coverage before analysis");
            }
        }
    }

    // The diagnostic fill is always calculated from source elevation, never
    // from a breached or routed surface. That keeps physical storage metrics
    // comparable across all conditioning modes.
    const std::vector<float> filled = priorityFlood(grid, grid.elevations_m,
                                                    !options.reject_interior_nodata,
                                                    checkCancellation);
    result.potential_fill_depth_m.assign(std::size_t(count),
        std::numeric_limits<float>::quiet_NaN());
    result.breach_cut_depth_m.assign(std::size_t(count), 0.0f);
    for (std::uint64_t index = 0; index < count; ++index)
        if (!validCell(grid, index))
            result.breach_cut_depth_m[std::size_t(index)] =
                std::numeric_limits<float>::quiet_NaN();

    if (options.breach_depressions)
    {
        if (!std::isfinite(options.maximum_breach_cut_depth_m) ||
            options.maximum_breach_cut_depth_m <= 0.0 ||
            !std::isfinite(options.maximum_breach_excavation_m3) ||
            options.maximum_breach_excavation_m3 <= 0.0 ||
            options.maximum_breach_path_cells < 2 ||
            options.maximum_breach_path_cells > 32 ||
            options.maximum_breach_candidates == 0)
            throw std::invalid_argument("Invalid constrained breach limits");
        carveConstrainedBreaches(grid, filled, options, result, checkCancellation);
    }
    for (std::uint64_t index = 0; index < count; ++index)
    {
        if ((index & 4095U) == 0) checkCancellation();
        if (!validCell(grid, index))
            continue;
        const float depth = filled[std::size_t(index)] - grid.elevations_m[std::size_t(index)];
        result.potential_fill_depth_m[std::size_t(index)] = depth;
        if (depth > 0.0f)
        {
            ++result.potential_fill_cell_count;
            result.maximum_potential_fill_m = std::max(result.maximum_potential_fill_m, depth);
        }
    }
    if (options.fill_depressions)
    {
        result.conditioned_elevations_m = options.breach_depressions
            ? priorityFlood(grid, result.conditioned_elevations_m,
                            !options.reject_interior_nodata, checkCancellation)
            : filled;
    }
    for (std::uint64_t index = 0; index < count; ++index)
    {
        if ((index & 4095U) == 0) checkCancellation();
        if (!validCell(grid, index))
            continue;
        const float source = grid.elevations_m[std::size_t(index)];
        const float conditioned = result.conditioned_elevations_m[std::size_t(index)];
        result.breach_cut_depth_m[std::size_t(index)] = std::max(0.0f, source - conditioned);
        if (conditioned > source)
        {
            ++result.conditioned_cell_count;
            result.maximum_fill_m = std::max(result.maximum_fill_m, conditioned - source);
        }
        if (conditioned < source)
        {
            ++result.breached_cell_count;
            const float cut = source - conditioned;
            result.maximum_breach_cut_m = std::max(result.maximum_breach_cut_m, cut);
            result.breach_excavation_m3 += double(cut) * rasterRowCellAreaM2(
                grid.geometry, grid.columns, std::uint32_t(index / grid.columns));
        }
    }

    // Compute actual downhill gradients and seed flat-routing from each cell
    // that already has a strictly lower neighbor or an open domain boundary.
    // Multi-source BFS then propagates routes through same-height plateaus
    // without changing the elevations. A closed flat depression left intact
    // has one deterministic sink per connected flat component.
    std::array<double, 8> row_distances = {};
    double row_area = 0.0;
    std::uint64_t cached_row = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::uint8_t> flat_assigned(std::size_t(count), 0);
    std::queue<std::uint64_t> flat_ready;
    for (std::uint64_t index = 0; index < count; ++index)
    {
        if ((index & 4095U) == 0) checkCancellation();
        if (!validCell(grid, index))
            continue;
        const std::uint64_t row = index / grid.columns;
        const std::uint64_t column = index % grid.columns;
        if (row != cached_row)
        {
            cached_row = row;
            row_area = rasterRowCellAreaM2(grid.geometry, grid.columns,
                                            std::uint32_t(row));
            for (int direction = 0; direction < 8; ++direction)
                row_distances[std::size_t(direction)] = rasterRowNeighborDistanceM(
                    grid.geometry, grid.columns, std::uint32_t(row),
                    NeighborDx[direction], NeighborDy[direction]);
        }
        const double depth = result.potential_fill_depth_m[std::size_t(index)];
        result.potential_fill_volume_m3 += depth * row_area;
        const double elevation = result.conditioned_elevations_m[std::size_t(index)];
        double highest_gradient = 0.0;
        std::int64_t best = -1;
        bool touches_open_boundary = column == 0 || row == 0 ||
            column + 1 == grid.columns || row + 1 == grid.rows;
        for (int direction = 0; direction < 8; ++direction)
        {
            std::uint64_t adjacent = 0;
            if (!neighbor(grid, index, direction, &adjacent))
                continue;
            if (!validCell(grid, adjacent))
            {
                if (!options.reject_interior_nodata)
                    touches_open_boundary = true;
                continue;
            }
            const double gradient = (elevation -
                result.conditioned_elevations_m[std::size_t(adjacent)]) /
                row_distances[std::size_t(direction)];
            if (gradient > highest_gradient)
            {
                highest_gradient = gradient;
                best = std::int64_t(adjacent);
            }
        }
        result.downstream[std::size_t(index)] = best;
        result.upstream_cell_count[std::size_t(index)] = 1;
        result.upstream_area_m2[std::size_t(index)] = row_area;
        if (best >= 0 || touches_open_boundary)
        {
            flat_assigned[std::size_t(index)] = 1;
            flat_ready.push(index);
        }
    }
    const std::function<void()> routeFlats = [&]()
    {
        std::uint64_t steps = 0;
        while (!flat_ready.empty())
        {
            if ((++steps & 4095U) == 0) checkCancellation();
            const std::uint64_t current = flat_ready.front();
            flat_ready.pop();
            for (int direction = 0; direction < 8; ++direction)
            {
                std::uint64_t adjacent = 0;
                if (!neighbor(grid, current, direction, &adjacent) ||
                    flat_assigned[std::size_t(adjacent)] || !validCell(grid, adjacent) ||
                    result.conditioned_elevations_m[std::size_t(current)] !=
                        result.conditioned_elevations_m[std::size_t(adjacent)])
                    continue;
                flat_assigned[std::size_t(adjacent)] = 1;
                result.downstream[std::size_t(adjacent)] = std::int64_t(current);
                flat_ready.push(adjacent);
            }
        }
    };
    routeFlats();
    // A remaining equal-height component cannot reach any strictly lower
    // cell or any boundary: preserve one true internal sink at its first
    // (row-major) cell, and route the rest of that component to the sink.
    for (std::uint64_t index = 0; index < count; ++index)
    {
        if ((index & 4095U) == 0) checkCancellation();
        if (!validCell(grid, index) || flat_assigned[std::size_t(index)])
            continue;
        flat_assigned[std::size_t(index)] = 1;
        flat_ready.push(index);
        routeFlats();
    }

    std::vector<std::uint8_t> inbound_count(std::size_t(count), 0);
    for (std::uint64_t index = 0; index < count; ++index)
    {
        const std::int64_t next = result.downstream[std::size_t(index)];
        if (next >= 0)
            ++inbound_count[std::size_t(next)];
    }
    std::queue<std::uint64_t> ready;
    std::uint64_t valid_count = 0;
    for (std::uint64_t index = 0; index < count; ++index)
    {
        if (!validCell(grid, index))
            continue;
        ++valid_count;
        if (inbound_count[std::size_t(index)] == 0)
            ready.push(index);
    }
    std::uint64_t processed_count = 0;
    while (!ready.empty())
    {
        if ((processed_count & 4095U) == 0) checkCancellation();
        const std::uint64_t index = ready.front();
        ready.pop();
        ++processed_count;
        const std::int64_t next = result.downstream[std::size_t(index)];
        if (next < 0)
            continue;
        result.upstream_cell_count[std::size_t(next)] +=
            result.upstream_cell_count[std::size_t(index)];
        result.upstream_area_m2[std::size_t(next)] +=
            result.upstream_area_m2[std::size_t(index)];
        if (--inbound_count[std::size_t(next)] == 0)
            ready.push(std::uint64_t(next));
    }
    if (processed_count != valid_count)
        throw std::runtime_error("Flow graph has a cycle (unexpected terrain-conditioning error)");
    return result;
}

std::vector<std::uint8_t> delineateCatchment(const AnalysisResult &result,
                                             std::uint64_t outlet_index)
{
    const std::size_t count = result.downstream.size();
    if (outlet_index >= count ||
        result.upstream_cell_count.size() != count ||
        result.upstream_cell_count[std::size_t(outlet_index)] == 0)
        throw std::invalid_argument("Catchment outlet lies outside the valid DEM");
    std::vector<std::uint8_t> mask(count, 0);
    std::queue<std::uint64_t> frontier;
    mask[std::size_t(outlet_index)] = 1;
    frontier.push(outlet_index);
    while (!frontier.empty())
    {
        const std::uint64_t current = frontier.front();
        frontier.pop();
        for (int direction = 0; direction < 8; ++direction)
        {
            std::uint64_t adjacent = 0;
            if (!neighbor(result.source, current, direction, &adjacent) ||
                mask[std::size_t(adjacent)] ||
                result.downstream[std::size_t(adjacent)] != std::int64_t(current))
                continue;
            mask[std::size_t(adjacent)] = 1;
            frontier.push(adjacent);
        }
    }
    return mask;
}

void cellCoordinate(const ElevationGrid &grid, std::uint64_t index,
                    double *x, double *y)
{
    if (index >= std::uint64_t(grid.columns) * grid.rows ||
        x == nullptr || y == nullptr)
        throw std::invalid_argument("Invalid cell index or coordinate output pointer");
    const MapCoordinate point = pixelCentre(grid.geometry,
        std::uint32_t(index % grid.columns), std::uint32_t(index / grid.columns));
    *x = point.x;
    *y = point.y;
}

} // namespace Aowis::Hydrology
