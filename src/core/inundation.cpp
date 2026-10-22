#include <aowis/hydrology/inundation.h>

#include <aowis/hydrology/geometry.h>

#include <algorithm>
#include <cmath>
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

void checkCancellation(const std::atomic<bool> *cancel_requested)
{
    if (cancel_requested != nullptr && cancel_requested->load(std::memory_order_relaxed))
        throw std::runtime_error("Static inundation reconstruction canceled");
}

bool nearVolume(double left, double right)
{
    return std::abs(left - right) <= 1e-7 * std::max({1.0, std::abs(left), std::abs(right)});
}

struct Cell
{
    std::uint64_t index = 0;
    double height = 0.0;
    double area = 0.0;
};

// Enumerate the pre-spill connected basin ONCE. This includes both sides of
// any already formed saddle, but not the spill saddle itself or an exterior
// cell. Do not use conditioned (possibly breached) elevations here.
std::vector<Cell> connectedBelowSpill(
    const ElevationGrid &dem, const DepressionBasin &basin,
    std::vector<std::uint32_t> &visited, std::uint32_t stamp,
    std::uint64_t *budget, const std::atomic<bool> *cancel_requested)
{
    const std::uint64_t count = std::uint64_t(dem.columns) * dem.rows;
    if (basin.minimum_cell >= count ||
        !(double(dem.elevations_m[std::size_t(basin.minimum_cell)]) < basin.spill_elevation_m))
        throw std::invalid_argument("Basin minimum is not below its spill elevation");
    std::vector<Cell> result;
    std::queue<std::uint64_t> ready;
    ready.push(basin.minimum_cell);
    visited[std::size_t(basin.minimum_cell)] = stamp;
    while (!ready.empty())
    {
        if (result.size() % 4096 == 0) checkCancellation(cancel_requested);
        if (*budget == 0)
            throw std::runtime_error("Static inundation cell budget exceeded");
        --*budget;
        const std::uint64_t index = ready.front();
        ready.pop();
        const double height = dem.elevations_m[std::size_t(index)];
        const double area = rasterRowCellAreaM2(dem.geometry, dem.columns,
            std::uint32_t(index / dem.columns));
        result.push_back({index, height, area});
        const std::int64_t x = std::int64_t(index % dem.columns);
        const std::int64_t y = std::int64_t(index / dem.columns);
        for (int direction = 0; direction < 8; ++direction)
        {
            const std::int64_t nx = x + Dx[direction];
            const std::int64_t ny = y + Dy[direction];
            if (nx < 0 || ny < 0 || nx >= dem.columns || ny >= dem.rows)
                continue;
            const std::uint64_t next = std::uint64_t(ny) * dem.columns + std::uint64_t(nx);
            if (visited[std::size_t(next)] == stamp ||
                !(double(dem.elevations_m[std::size_t(next)]) < basin.spill_elevation_m))
                continue;
            visited[std::size_t(next)] = stamp;
            ready.push(next);
        }
    }
    return result;
}

// Exact piecewise-linear inversion of V(h) = sum(area_i * max(0,h-z_i)).
// No iterative basin flood or floating-point cancellation of absolute level
// times total area. Height groups are deterministic for identical DEM input.
double levelForVolume(std::vector<Cell> &cells, double volume, double spill)
{
    std::sort(cells.begin(), cells.end(), [](const Cell &left, const Cell &right)
    {
        return left.height == right.height ? left.index < right.index :
            left.height < right.height;
    });
    double level = cells.front().height;
    double cumulative_area = 0.0;
    double cumulative_volume = 0.0;
    std::size_t index = 0;
    while (index < cells.size())
    {
        const double next_height = cells[index].height;
        const double additional = cumulative_area * (next_height - level);
        if (volume <= cumulative_volume + additional && cumulative_area > 0.0)
            return level + std::max(0.0, (volume - cumulative_volume) / cumulative_area);
        cumulative_volume += additional;
        level = next_height;
        while (index < cells.size() && cells[index].height == next_height)
            cumulative_area += cells[index++].area;
    }
    if (cumulative_area <= 0.0)
        throw std::runtime_error("Invalid basin area");
    const double upper_volume = cumulative_volume + cumulative_area * (spill - level);
    if (!nearVolume(upper_volume, volume) && volume > upper_volume)
        throw std::invalid_argument("Stored volume exceeds basin topographic capacity");
    return std::min(spill, level + std::max(0.0, volume - cumulative_volume) / cumulative_area);
}

} // namespace

StaticInundation reconstructStaticInundation(
    const ElevationGrid &original_dem,
    const DepressionHierarchy &hierarchy,
    const RunoffRedistribution &redistribution,
    std::uint64_t maximum_visited_cells,
    const std::atomic<bool> *cancel_requested)
{
    checkCancellation(cancel_requested);
    const std::uint64_t count = std::uint64_t(original_dem.columns) * original_dem.rows;
    if (count == 0 || original_dem.columns < 2 || original_dem.rows < 2 ||
        maximum_visited_cells == 0 || original_dem.elevations_m.size() != count ||
        hierarchy.columns != original_dem.columns || hierarchy.rows != original_dem.rows ||
        hierarchy.valid_cells != count ||
        redistribution.basins.size() != hierarchy.basins.size())
        throw std::invalid_argument("Mismatched static inundation input grids and basin states");
    validateRasterGeometry(original_dem.geometry, original_dem.columns, original_dem.rows);
    for (std::uint64_t cell = 0; cell < count; ++cell)
        if (!std::isfinite(original_dem.elevations_m[std::size_t(cell)]))
            throw std::invalid_argument("Static inundation requires complete original DEM coverage");
    std::vector<std::uint64_t> active_roots;
    double accepted_storage = 0.0;
    double all_roots_storage = 0.0;
    bool ambiguous = false;
    for (std::size_t i = 0; i < hierarchy.basins.size(); ++i)
    {
        const DepressionBasin &basin = hierarchy.basins[i];
        const BasinRedistributionState &state = redistribution.basins[i];
        if (basin.index != i || state.basin_index != i ||
            basin.minimum_cell >= count || basin.spill_cell >= count ||
            double(original_dem.elevations_m[std::size_t(basin.minimum_cell)]) !=
                basin.minimum_elevation_m ||
            double(original_dem.elevations_m[std::size_t(basin.spill_cell)]) !=
                basin.spill_elevation_m ||
            !std::isfinite(state.stored_in_subtree_m3) ||
            state.stored_in_subtree_m3 < 0.0 ||
            !std::isfinite(basin.capacity_at_spill_m3) ||
            !std::isfinite(basin.spill_elevation_m) ||
            basin.capacity_at_spill_m3 < 0.0 ||
            state.stored_in_subtree_m3 > basin.capacity_at_spill_m3 +
                1e-7 * std::max(1.0, basin.capacity_at_spill_m3))
            throw std::invalid_argument("Invalid basin storage state for static inundation");
        if (basin.parent_index >= 0) continue;
        const double volume = state.stored_in_subtree_m3;
        all_roots_storage += volume;
        if (volume <= 0.0) continue;
        double formation_volume = 0.0;
        for (const std::uint64_t child_index : basin.child_indices)
        {
            if (child_index >= hierarchy.basins.size() ||
                hierarchy.basins[std::size_t(child_index)].parent_index != std::int64_t(i))
                throw std::invalid_argument("Invalid basin child relationship");
            const double cap = hierarchy.basins[std::size_t(child_index)].capacity_at_spill_m3;
            formation_volume += cap;
            if (!nearVolume(redistribution.basins[std::size_t(child_index)].stored_in_subtree_m3, cap))
                ambiguous = true;
        }
        if (!basin.child_indices.empty() && volume +
            1e-7 * std::max(1.0, formation_volume) < formation_volume)
            ambiguous = true;
        if (ambiguous) continue;
        accepted_storage += volume;
        active_roots.push_back(std::uint64_t(i));
    }
    StaticInundation result;
    if (ambiguous)
    {
        result.status = InundationStatus::UnresolvedPartialMerge;
        return result; // No raster, even if only one root is unresolved.
    }
    if (!nearVolume(all_roots_storage, redistribution.basin_storage_m3) ||
        !nearVolume(accepted_storage, redistribution.basin_storage_m3))
        throw std::invalid_argument("Static inundation does not match retained basin volume");
    if (count > maximum_visited_cells)
        throw std::runtime_error("Static inundation raster exceeds cell budget");
    result.depth_m.assign(std::size_t(count), 0.0f);
    std::vector<std::uint32_t> visited(std::size_t(count), 0);
    std::uint32_t stamp = 0;
    std::uint64_t budget = maximum_visited_cells;
    for (const std::uint64_t root_index : active_roots)
    {
        checkCancellation(cancel_requested);
        if (stamp == std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("Too many independent inundation basins");
        ++stamp;
        const DepressionBasin &basin = hierarchy.basins[std::size_t(root_index)];
        const BasinRedistributionState &state = redistribution.basins[std::size_t(root_index)];
        std::vector<Cell> cells = connectedBelowSpill(original_dem, basin, visited, stamp,
            &budget, cancel_requested);
        double capacity = 0.0;
        for (const Cell &cell : cells)
            capacity += (basin.spill_elevation_m - cell.height) * cell.area;
        if (!nearVolume(capacity, basin.capacity_at_spill_m3))
            throw std::invalid_argument("Connected DEM basin volume disagrees with hierarchy");
        const double level = levelForVolume(cells, state.stored_in_subtree_m3,
                                           basin.spill_elevation_m);
        if (!basin.child_indices.empty() &&
            level + 1e-9 < basin.formation_elevation_m)
            throw std::invalid_argument("Merged basin water surface is below its formation saddle");
        BasinWaterSurface surface;
        surface.basin_index = root_index;
        surface.stored_volume_m3 = state.stored_in_subtree_m3;
        surface.water_level_m = level;
        double physically_stored_m3 = 0.0;
        double raster_stored_m3 = 0.0;
        for (const Cell &cell : cells)
        {
            const double depth = std::max(0.0, level - cell.height);
            if (depth <= 0.0) continue;
            if (result.depth_m[std::size_t(cell.index)] > 0.0f)
                throw std::runtime_error("Independent basins have overlapping inundation");
            if (depth > std::numeric_limits<float>::max())
                throw std::runtime_error("Inundation depth exceeds float32 range");
            result.depth_m[std::size_t(cell.index)] = float(depth);
            physically_stored_m3 += depth * cell.area;
            raster_stored_m3 += double(result.depth_m[std::size_t(cell.index)]) * cell.area;
            surface.inundated_area_m2 += cell.area;
            ++surface.inundated_cells;
        }
        if (!nearVolume(physically_stored_m3, state.stored_in_subtree_m3) ||
            std::abs(raster_stored_m3 - state.stored_in_subtree_m3) >
                5e-6 * std::max(1.0, state.stored_in_subtree_m3))
            throw std::runtime_error("Static inundation depth raster fails volume balance");
        result.reconstructed_volume_m3 += physically_stored_m3;
        result.inundated_area_m2 += surface.inundated_area_m2;
        result.inundated_cells += surface.inundated_cells;
        result.surfaces.push_back(surface);
    }
    if (!nearVolume(result.reconstructed_volume_m3, redistribution.basin_storage_m3))
        throw std::runtime_error("Reconstructed inundation volume does not match stored runoff");
    return result;
}

} // namespace Aowis::Hydrology
