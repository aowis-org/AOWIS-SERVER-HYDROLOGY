#include <aowis/hydrology/redistribution.h>

#include <algorithm>
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
constexpr int Dx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int Dy[8] = {0, 1, 1, 1, 0, -1, -1, -1};

void checkCancellation(const std::atomic<bool> *cancel)
{
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
        throw std::runtime_error("Runoff redistribution cancelled");
}

bool boundaryCell(const ElevationGrid &grid, std::uint64_t cell)
{
    const std::uint64_t x = cell % grid.columns;
    const std::uint64_t y = cell / grid.columns;
    return x == 0 || y == 0 || x + 1 == grid.columns || y + 1 == grid.rows;
}

// Put incremental water into a subtree. Iterative traversal avoids call-stack
// exhaustion for deeply nested terrain; children are filled before their
// parent accumulates the volume above the saddle. Sibling allocation is
// deterministic, NOT a simulation of velocities through individual spillways.
double addToSubtree(std::uint64_t index, double water,
                    const DepressionHierarchy &hierarchy,
                    std::vector<BasinRedistributionState> &states,
                    std::uint64_t *visit_budget)
{
    if (water <= 0.0) return 0.0;
    struct Frame
    {
        std::uint64_t basin;
        double accepted;
        double remaining_for_children;
        std::size_t next_child = 0;
    };
    const std::size_t count = hierarchy.basins.size();
    std::vector<Frame> stack;
    const std::function<void(std::uint64_t, double)> push =
        [&](std::uint64_t basin_index, double available)
    {
        if (*visit_budget == 0 || basin_index >= count)
            throw std::runtime_error("Invalid or cyclic depression hierarchy");
        --*visit_budget;
        const double cap = hierarchy.basins[std::size_t(basin_index)].capacity_at_spill_m3;
        if (!std::isfinite(cap) || cap < 0.0)
            throw std::invalid_argument("Runoff reached a basin without finite spill capacity");
        const double remaining = std::max(0.0,
            cap - states[std::size_t(basin_index)].stored_in_subtree_m3);
        const double accepted = std::min(available, remaining);
        stack.push_back({basin_index, accepted, accepted, 0});
    };
    push(index, water);
    double finished = 0.0;
    while (!stack.empty())
    {
        Frame &top = stack.back();
        const DepressionBasin &basin = hierarchy.basins[std::size_t(top.basin)];
        if (top.remaining_for_children > 0.0 &&
            top.next_child < basin.child_indices.size())
        {
            const std::uint64_t child = basin.child_indices[top.next_child++];
            push(child, top.remaining_for_children);
            continue;
        }
        BasinRedistributionState &state = states[std::size_t(top.basin)];
        state.stored_in_subtree_m3 += top.accepted;
        const double cap = basin.capacity_at_spill_m3;
        state.at_spill = cap - state.stored_in_subtree_m3 <=
            1e-10 * std::max(1.0, cap);
        finished = top.accepted;
        stack.pop_back();
        if (!stack.empty())
            stack.back().remaining_for_children = std::max(
                0.0, stack.back().remaining_for_children - finished);
    }
    return finished;
}
}

RunoffRedistribution redistributeRunoff(
    const ElevationGrid &original_dem, const DepressionHierarchy &hierarchy,
    const std::vector<double> &runoff_volume_m3,
    const std::atomic<bool> *cancel_requested)
{
    checkCancellation(cancel_requested);
    const std::uint64_t cells = std::uint64_t(original_dem.columns) * original_dem.rows;
    if (original_dem.columns < 2 || original_dem.rows < 2 ||
        cells != original_dem.elevations_m.size() ||
        cells != runoff_volume_m3.size() ||
        hierarchy.columns != original_dem.columns ||
        hierarchy.rows != original_dem.rows || hierarchy.valid_cells != cells)
        throw std::invalid_argument("Runoff, elevation and depression grids do not match");
    for (std::uint64_t cell = 0; cell < cells; ++cell)
    {
        if ((cell & 4095U) == 0) checkCancellation(cancel_requested);
        if (!std::isfinite(original_dem.elevations_m[std::size_t(cell)]) ||
            !std::isfinite(runoff_volume_m3[std::size_t(cell)]) ||
            runoff_volume_m3[std::size_t(cell)] < 0.0)
            throw std::invalid_argument("Invalid elevation coverage or runoff volume");
    }
    const std::size_t basin_count = hierarchy.basins.size();
    std::vector<std::uint8_t> seen(basin_count, 0);
    for (std::size_t i = 0; i < basin_count; ++i)
    {
        const DepressionBasin &basin = hierarchy.basins[i];
        if (basin.index != i || basin.minimum_cell >= cells ||
            !std::isfinite(basin.capacity_at_spill_m3) ||
            basin.capacity_at_spill_m3 < 0.0 ||
            !std::isfinite(basin.spill_elevation_m) ||
            (basin.parent_index >= 0 && std::uint64_t(basin.parent_index) <= i) ||
            (basin.parent_index < 0 && !basin.spills_to_domain_boundary))
            throw std::invalid_argument("Invalid or unresolved depression hierarchy");
        if (basin.spill_cell >= cells ||
            double(original_dem.elevations_m[std::size_t(basin.minimum_cell)]) !=
                basin.minimum_elevation_m ||
            double(original_dem.elevations_m[std::size_t(basin.spill_cell)]) !=
                basin.spill_elevation_m)
            throw std::invalid_argument("Depression hierarchy does not match the source DEM");
        if (basin.parent_index >= 0 &&
            std::uint64_t(basin.parent_index) >= basin_count)
            throw std::invalid_argument("Depression parent index is outside the hierarchy");
        for (const std::uint64_t child : basin.child_indices)
        {
            if (child >= i || ++seen[std::size_t(child)] != 1 ||
                hierarchy.basins[std::size_t(child)].parent_index != std::int64_t(i))
                throw std::invalid_argument("Invalid depression child relationship");
        }
    }
    for (std::size_t i = 0; i < basin_count; ++i)
        if ((hierarchy.basins[i].parent_index >= 0 && seen[i] != 1) ||
            (hierarchy.basins[i].parent_index < 0 && seen[i] != 0))
            throw std::invalid_argument("Invalid depression hierarchy linkage");

    // Identify the connected minimum plateau for every leaf. Flat routing
    // chooses a deterministic sink which need not equal the hierarchy's
    // minimum_cell, so match the entire equal-elevation plateau.
    std::vector<std::int64_t> minimum_owner(std::size_t(cells), -1);
    std::queue<std::uint64_t> frontier;
    for (std::size_t i = 0; i < basin_count; ++i)
    {
        const DepressionBasin &basin = hierarchy.basins[i];
        if (!basin.child_indices.empty()) continue;
        const std::uint64_t minimum = basin.minimum_cell;
        if (minimum_owner[std::size_t(minimum)] >= 0)
            throw std::invalid_argument("Overlapping basin minimum plateaus");
        minimum_owner[std::size_t(minimum)] = std::int64_t(i);
        frontier.push(minimum);
        while (!frontier.empty())
        {
            const std::uint64_t current = frontier.front();
            frontier.pop();
            const std::int64_t x = std::int64_t(current % original_dem.columns);
            const std::int64_t y = std::int64_t(current / original_dem.columns);
            for (int direction = 0; direction < 8; ++direction)
            {
                const std::int64_t nx = x + Dx[direction];
                const std::int64_t ny = y + Dy[direction];
                if (nx < 0 || ny < 0 || nx >= original_dem.columns || ny >= original_dem.rows)
                    continue;
                const std::uint64_t next = std::uint64_t(ny) * original_dem.columns + std::uint64_t(nx);
                if (minimum_owner[std::size_t(next)] >= 0 ||
                    original_dem.elevations_m[std::size_t(next)] !=
                        original_dem.elevations_m[std::size_t(minimum)]) continue;
                minimum_owner[std::size_t(next)] = std::int64_t(i);
                frontier.push(next);
            }
        }
    }

    AnalysisOptions dry_options;
    dry_options.fill_depressions = false;
    dry_options.breach_depressions = false;
    dry_options.reject_interior_nodata = true;
    const AnalysisResult flow = analyze(original_dem, dry_options, cancel_requested);
    std::vector<std::uint8_t> incoming(std::size_t(cells), 0);
    std::vector<double> moving = runoff_volume_m3;
    RunoffRedistribution result;
    result.basins.resize(basin_count);
    for (std::size_t i = 0; i < basin_count; ++i)
        result.basins[i].basin_index = std::uint64_t(i);
    for (std::uint64_t cell = 0; cell < cells; ++cell)
    {
        if ((cell & 4095U) == 0) checkCancellation(cancel_requested);
        result.generated_runoff_m3 += moving[std::size_t(cell)];
        const std::int64_t next = flow.downstream[std::size_t(cell)];
        if (next >= 0)
        {
            if (std::uint64_t(next) >= cells || ++incoming[std::size_t(next)] > 8)
                throw std::runtime_error("Invalid dry D8 drainage graph");
        }
    }
    std::queue<std::uint64_t> ready;
    for (std::uint64_t cell = 0; cell < cells; ++cell)
        if (incoming[std::size_t(cell)] == 0) ready.push(cell);
    std::uint64_t visited = 0;
    while (!ready.empty())
    {
        if ((++visited & 4095U) == 0) checkCancellation(cancel_requested);
        const std::uint64_t cell = ready.front();
        ready.pop();
        const std::int64_t next = flow.downstream[std::size_t(cell)];
        const double water = moving[std::size_t(cell)];
        if (next >= 0)
        {
            moving[std::size_t(next)] += water;
            if (--incoming[std::size_t(next)] == 0)
                ready.push(std::uint64_t(next));
        }
        else if (boundaryCell(original_dem, cell))
            result.direct_boundary_outflow_m3 += water;
        else
        {
            const std::int64_t owner = minimum_owner[std::size_t(cell)];
            if (owner < 0)
                throw std::runtime_error("Internal D8 sink is not a depression minimum");
            result.basins[std::size_t(owner)].direct_inflow_m3 += water;
        }
    }
    if (visited != cells)
        throw std::runtime_error("Dry D8 flow graph contains a cycle");

    // Basin indices are topological (children precede parents). A child
    // retains its capacity and passes excess to the next merge node, which
    // may fill sibling deficits before rising above the saddle itself.
    for (std::size_t i = 0; i < basin_count; ++i)
    {
        if ((i & 4095U) == 0) checkCancellation(cancel_requested);
        const DepressionBasin &basin = hierarchy.basins[i];
        BasinRedistributionState &state = result.basins[i];
        double available = state.direct_inflow_m3;
        for (const std::uint64_t child : basin.child_indices)
            available += result.basins[std::size_t(child)].overflow_m3;
        // The parent already includes water held in its descendants.
        for (const std::uint64_t child : basin.child_indices)
            state.stored_in_subtree_m3 += result.basins[std::size_t(child)].stored_in_subtree_m3;
        std::uint64_t budget = basin_count + 1;
        const double added = addToSubtree(std::uint64_t(i), available, hierarchy, result.basins, &budget);
        state.overflow_m3 = std::max(0.0, available - added);
        if (basin.parent_index < 0)
        {
            result.basin_storage_m3 += state.stored_in_subtree_m3;
            result.basin_boundary_outflow_m3 += state.overflow_m3;
        }
    }
    result.total_boundary_outflow_m3 = result.direct_boundary_outflow_m3 +
        result.basin_boundary_outflow_m3;
    result.mass_balance_error_m3 = result.generated_runoff_m3 -
        result.basin_storage_m3 - result.total_boundary_outflow_m3;
    const double magnitude = std::max(1.0, result.generated_runoff_m3);
    if (!std::isfinite(result.mass_balance_error_m3) ||
        std::abs(result.mass_balance_error_m3) > 1e-8 * magnitude)
        throw std::runtime_error("Runoff redistribution mass balance failure");
    return result;
}

} // namespace Aowis::Hydrology
