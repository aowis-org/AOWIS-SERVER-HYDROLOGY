#include <aowis/hydrology/depression_hierarchy.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace Aowis::Hydrology
{
namespace
{
constexpr int Dx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int Dy[8] = {0, 1, 1, 1, 0, -1, -1, -1};

bool adjacent(const ElevationGrid &grid, std::uint64_t index, int direction,
              std::uint64_t *target)
{
    const std::int64_t x = std::int64_t(index % grid.columns) + Dx[direction];
    const std::int64_t y = std::int64_t(index / grid.columns) + Dy[direction];
    if (x < 0 || y < 0 || x >= grid.columns || y >= grid.rows)
        return false;
    *target = std::uint64_t(y) * grid.columns + std::uint64_t(x);
    return true;
}

void checkCancelled(const std::atomic<bool> *cancel)
{
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
        throw std::runtime_error("Job canceled");
}

bool isBoundary(const ElevationGrid &grid, std::uint64_t index,
                bool treat_nodata_as_open)
{
    const std::uint64_t x = index % grid.columns;
    const std::uint64_t y = index / grid.columns;
    if (x == 0 || y == 0 || x + 1 == grid.columns || y + 1 == grid.rows)
        return true;
    if (treat_nodata_as_open)
    {
        for (int direction = 0; direction < 8; ++direction)
        {
            std::uint64_t other = 0;
            if (adjacent(grid, index, direction, &other) &&
                !std::isfinite(grid.elevations_m[std::size_t(other)]))
                return true;
        }
    }
    return false;
}

struct DisjointComponents
{
    explicit DisjointComponents(std::size_t count)
        : roots(count, -1), sizes(count, 0), areas(count, 0.0),
          minimum_heights(count, 0.0), relative_height_area(count, 0.0),
          open(count, 0), basin(count, -1) {}

    std::vector<std::int64_t> roots;
    std::vector<std::uint64_t> sizes;
    std::vector<double> areas;
    // The datum is the component minimum, avoiding cancellation of two
    // enormous absolute-height*area values for shallow basins.
    std::vector<double> minimum_heights;
    std::vector<double> relative_height_area;
    std::vector<std::uint8_t> open;
    std::vector<std::int64_t> basin;

    std::uint64_t find(std::uint64_t index)
    {
        std::uint64_t root = index;
        while (this->roots[std::size_t(root)] != std::int64_t(root))
            root = std::uint64_t(this->roots[std::size_t(root)]);
        while (index != root)
        {
            const std::uint64_t next = std::uint64_t(this->roots[std::size_t(index)]);
            this->roots[std::size_t(index)] = std::int64_t(root);
            index = next;
        }
        return root;
    }

    std::uint64_t unite(std::uint64_t left, std::uint64_t right)
    {
        left = find(left);
        right = find(right);
        if (left == right) return left;
        if (this->sizes[std::size_t(left)] < this->sizes[std::size_t(right)] ||
            (this->sizes[std::size_t(left)] == this->sizes[std::size_t(right)] && right < left))
            std::swap(left, right);
        this->roots[std::size_t(right)] = std::int64_t(left);
        this->sizes[std::size_t(left)] += this->sizes[std::size_t(right)];
        this->areas[std::size_t(left)] += this->areas[std::size_t(right)];
        const double reference = std::min(this->minimum_heights[std::size_t(left)],
                                          this->minimum_heights[std::size_t(right)]);
        this->relative_height_area[std::size_t(left)] +=
            this->relative_height_area[std::size_t(right)] +
            (this->minimum_heights[std::size_t(left)] - reference) *
                (this->areas[std::size_t(left)] - this->areas[std::size_t(right)]) +
            (this->minimum_heights[std::size_t(right)] - reference) *
                this->areas[std::size_t(right)];
        this->minimum_heights[std::size_t(left)] = reference;
        this->open[std::size_t(left)] |= this->open[std::size_t(right)];
        return left;
    }
};

// When the component first reaches its saddle, only cells BELOW the saddle
// have accumulated water. The new saddle cells themselves contribute zero.
void finishBasin(DepressionHierarchy &result, DisjointComponents &components,
                 std::uint64_t component_root, double saddle, std::uint64_t saddle_cell,
                 bool to_boundary, std::int64_t parent)
{
    const std::int64_t id = components.basin[std::size_t(component_root)];
    if (id < 0) return;
    DepressionBasin &basin = result.basins[std::size_t(id)];
    if (std::isfinite(basin.spill_elevation_m))
        throw std::runtime_error("Depression hierarchy attempted to finalize a basin twice");
    basin.spill_elevation_m = saddle;
    basin.spill_cell = saddle_cell;
    basin.spills_to_domain_boundary = to_boundary;
    basin.parent_index = parent;
    basin.capacity_at_spill_m3 = std::max(0.0,
        (saddle - components.minimum_heights[std::size_t(component_root)]) *
            components.areas[std::size_t(component_root)] -
        components.relative_height_area[std::size_t(component_root)]);
    basin.area_below_spill_m2 = components.areas[std::size_t(component_root)];
    basin.cells_below_spill = components.sizes[std::size_t(component_root)];
}

} // namespace

DepressionHierarchy buildDepressionHierarchy(
    const ElevationGrid &source, bool reject_interior_nodata,
    const std::atomic<bool> *cancel_requested)
{
    checkCancelled(cancel_requested);
    if (source.columns < 2 || source.rows < 2 ||
        std::uint64_t(source.columns) * source.rows != source.elevations_m.size())
        throw std::invalid_argument("Invalid source elevation raster for depression hierarchy");
    validateRasterGeometry(source.geometry, source.columns, source.rows);
    const std::uint64_t cell_count = std::uint64_t(source.columns) * source.rows;
    if (cell_count > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("Depression hierarchy raster too large");
    std::vector<std::uint64_t> sorted;
    sorted.reserve(std::size_t(cell_count));
    for (std::uint64_t index = 0; index < cell_count; ++index)
    {
        if ((index & 4095U) == 0) checkCancelled(cancel_requested);
        const float height = source.elevations_m[std::size_t(index)];
        if (!std::isfinite(height))
        {
            if (reject_interior_nodata && index / source.columns > 0 &&
                index / source.columns + 1 < source.rows &&
                index % source.columns > 0 && index % source.columns + 1 < source.columns)
                throw std::invalid_argument("Interior NoData in depression analysis domain");
            continue;
        }
        sorted.push_back(index);
    }
    std::sort(sorted.begin(), sorted.end(), [&source](std::uint64_t left, std::uint64_t right)
    {
        const float l = source.elevations_m[std::size_t(left)];
        const float r = source.elevations_m[std::size_t(right)];
        return l == r ? left < right : l < r;
    });

    // A raster row has one metric cell area under the current raster
    // geometry contract. GeographicLib polygon evaluation is expensive;
    // calculating it once per cell was the dominant work on real DEMs.
    // Retain the identical geodesic calculation, just reuse its result.
    std::vector<double> row_cell_areas(std::size_t(source.rows), 0.0);
    for (std::uint32_t row = 0; row < source.rows; ++row)
    {
        if ((row & 255U) == 0) checkCancelled(cancel_requested);
        row_cell_areas[std::size_t(row)] =
            rasterRowCellAreaM2(source.geometry, source.columns, row);
    }

    DisjointComponents components{std::size_t(cell_count)};
    DepressionHierarchy result;
    result.valid_cells = sorted.size();
    result.columns = source.columns;
    result.rows = source.rows;
    std::vector<std::uint32_t> activation_stamp(std::size_t(cell_count), 0);
    std::uint32_t stamp = 0;
    // Reuse the small vectors across basins. A real-world DEM can have
    // thousands of individual saddle elevations and local minima.
    std::vector<std::uint64_t> roots;
    std::vector<std::uint64_t> closed;
    roots.reserve(16);
    closed.reserve(8);
    const auto processPlateau = [&](std::uint64_t plateau_key,
                                    const std::vector<std::uint64_t> &pixels,
                                    float height)
    {
        checkCancelled(cancel_requested);
        std::uint64_t plateau = components.find(plateau_key);
        roots.clear();
        roots.push_back(plateau);
        for (const std::uint64_t index : pixels)
            for (int direction = 0; direction < 8; ++direction)
            {
                std::uint64_t other = 0;
                if (adjacent(source, index, direction, &other) &&
                    components.roots[std::size_t(other)] >= 0 &&
                    activation_stamp[std::size_t(other)] != stamp)
                    roots.push_back(components.find(other));
            }
        for (std::uint64_t &root : roots)
            root = components.find(root);
        std::sort(roots.begin(), roots.end());
        roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
        const std::uint64_t saddle_cell = pixels.front();
        bool open = false;
        closed.clear();
        for (const std::uint64_t root : roots)
        {
            if (components.open[std::size_t(root)] != 0)
                open = true;
            else if (components.basin[std::size_t(root)] >= 0)
                closed.push_back(root);
        }

        std::int64_t next_basin = -1;
        if (open)
        {
            for (const std::uint64_t root : closed)
                finishBasin(result, components, root, double(height), saddle_cell, true, -1);
        }
        else if (closed.size() >= 2)
        {
            const std::uint64_t id = result.basins.size();
            DepressionBasin parent;
            parent.index = id;
            parent.spill_cell = saddle_cell;
            parent.formation_elevation_m = height;
            parent.minimum_elevation_m = std::numeric_limits<double>::infinity();
            for (const std::uint64_t root : closed)
            {
                const std::uint64_t child_id = std::uint64_t(components.basin[std::size_t(root)]);
                const DepressionBasin &child = result.basins[std::size_t(child_id)];
                if (child.minimum_elevation_m < parent.minimum_elevation_m)
                {
                    parent.minimum_elevation_m = child.minimum_elevation_m;
                    parent.minimum_cell = child.minimum_cell;
                }
                parent.child_indices.push_back(child_id);
            }
            for (const std::uint64_t root : closed)
                finishBasin(result, components, root, double(height), saddle_cell,
                            false, std::int64_t(id));
            result.basins.push_back(std::move(parent));
            next_basin = std::int64_t(id);
        }
        else if (closed.size() == 1)
            next_basin = components.basin[std::size_t(closed.front())];
        else if (!open)
        {
            // Newly appearing isolated flat plateau: local minimum.
            next_basin = std::int64_t(result.basins.size());
            DepressionBasin leaf;
            leaf.index = std::uint64_t(next_basin);
            leaf.minimum_cell = pixels.front();
            leaf.minimum_elevation_m = height;
            leaf.formation_elevation_m = height;
            leaf.spill_cell = pixels.front();
            result.basins.push_back(std::move(leaf));
        }
        for (const std::uint64_t root : roots)
            plateau = components.unite(plateau, root);
        plateau = components.find(plateau);
        components.basin[std::size_t(plateau)] = open ? -1 : next_basin;
        components.open[std::size_t(plateau)] = open ? 1 : 0;
    };
    std::vector<std::uint64_t> singleton_plateau(1);
    std::size_t first = 0;
    while (first < sorted.size())
    {
        checkCancelled(cancel_requested);
        std::size_t end = first + 1;
        const float height = source.elevations_m[std::size_t(sorted[first])];
        while (end < sorted.size() && source.elevations_m[std::size_t(sorted[end])] == height)
            ++end;
        ++stamp;
        for (std::size_t at = first; at < end; ++at)
        {
            const std::uint64_t index = sorted[at];
            components.roots[std::size_t(index)] = std::int64_t(index);
            components.sizes[std::size_t(index)] = 1;
            const double area = row_cell_areas[std::size_t(index / source.columns)];
            components.areas[std::size_t(index)] = area;
            components.minimum_heights[std::size_t(index)] = height;
            components.relative_height_area[std::size_t(index)] = 0.0;
            components.open[std::size_t(index)] = isBoundary(
                source, index, !reject_interior_nodata) ? 1 : 0;
            if (components.open[std::size_t(index)] != 0)
                result.has_open_boundary = true;
            activation_stamp[std::size_t(index)] = stamp;
        }
        // A unique elevation is already a one-cell plateau. Avoid constructing
        // a hash map and sorting plateau groups in this extremely common case;
        // retain precisely the same saddle and component handling as above.
        if (end == first + 1)
        {
            singleton_plateau.front() = sorted[first];
            processPlateau(sorted[first], singleton_plateau, height);
            first = end;
            continue;
        }
        // Process equal-elevation plateaus together. This avoids inventing
        // an individual basin for every pixel in a flat depression floor.
        for (std::size_t at = first; at < end; ++at)
        {
            const std::uint64_t index = sorted[at];
            for (int direction = 0; direction < 8; ++direction)
            {
                std::uint64_t other = 0;
                if (adjacent(source, index, direction, &other) &&
                    activation_stamp[std::size_t(other)] == stamp)
                    components.unite(index, other);
            }
        }
        std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> plateaus;
        plateaus.reserve(end - first);
        for (std::size_t at = first; at < end; ++at)
        {
            const std::uint64_t index = sorted[at];
            plateaus[components.find(index)].push_back(index);
        }
        // Stable ordering is important for basin IDs and reproducible tests.
        std::vector<std::uint64_t> plateau_roots;
        plateau_roots.reserve(plateaus.size());
        for (const std::pair<const std::uint64_t, std::vector<std::uint64_t>> &entry : plateaus)
            plateau_roots.push_back(entry.first);
        std::sort(plateau_roots.begin(), plateau_roots.end());
        for (const std::uint64_t plateau_key : plateau_roots)
            processPlateau(plateau_key, plateaus.at(plateau_key), height);
        first = end;
    }
    return result;
}

BasinStorageState basinStorageAtLevel(
    const ElevationGrid &source, const DepressionHierarchy &hierarchy,
    std::uint64_t basin_index, double water_level_m,
    std::uint64_t max_visited_cells,
    const std::atomic<bool> *cancel_requested)
{
    if (basin_index >= hierarchy.basins.size() ||
        source.columns != hierarchy.columns || source.rows != hierarchy.rows ||
        !std::isfinite(water_level_m) || max_visited_cells == 0 ||
        std::uint64_t(source.columns) * source.rows != source.elevations_m.size())
        throw std::invalid_argument("Invalid basin storage query");
    const DepressionBasin &basin = hierarchy.basins[std::size_t(basin_index)];
    if (!std::isfinite(basin.spill_elevation_m))
        throw std::invalid_argument("Basin does not have a known outlet within this terrain domain");
    if (water_level_m < basin.formation_elevation_m ||
        water_level_m > basin.spill_elevation_m)
        throw std::invalid_argument("Water level outside basin minimum/spill interval");
    if (water_level_m == basin.spill_elevation_m)
    {
        BasinStorageState result;
        result.water_level_m = water_level_m;
        result.water_volume_m3 = basin.capacity_at_spill_m3;
        result.inundated_cell_area_m2 = basin.area_below_spill_m2;
        result.inundated_cells = basin.cells_below_spill;
        return result;
    }
    const std::uint64_t count = std::uint64_t(source.columns) * source.rows;
    if (basin.minimum_cell >= count)
        throw std::invalid_argument("Basin minimum lies outside DEM");
    // Storage queries visit cells in arbitrary BFS order. Lazily calculate
    // each encountered row area once, so even a large connected lake doesn't
    // repeat ellipsoidal-polygon evaluation for every cell.
    std::vector<double> row_cell_areas(std::size_t(source.rows),
                                       std::numeric_limits<double>::quiet_NaN());
    std::vector<std::uint8_t> visited(std::size_t(count), 0);
    std::queue<std::uint64_t> ready;
    visited[std::size_t(basin.minimum_cell)] = 1;
    ready.push(basin.minimum_cell);
    BasinStorageState result;
    result.water_level_m = water_level_m;
    while (!ready.empty())
    {
        if ((result.inundated_cells & 4095U) == 0) checkCancelled(cancel_requested);
        const std::uint64_t index = ready.front();
        ready.pop();
        if (++result.inundated_cells > max_visited_cells)
            throw std::runtime_error("Basin storage query cell limit exceeded");
        const double elevation = source.elevations_m[std::size_t(index)];
        const std::size_t row = std::size_t(index / source.columns);
        double &area = row_cell_areas[row];
        if (std::isnan(area))
            area = rasterRowCellAreaM2(source.geometry, source.columns,
                                       std::uint32_t(row));
        result.inundated_cell_area_m2 += area;
        result.water_volume_m3 += std::max(0.0, water_level_m - elevation) * area;
        for (int direction = 0; direction < 8; ++direction)
        {
            std::uint64_t other = 0;
            if (!adjacent(source, index, direction, &other) || visited[std::size_t(other)] ||
                !std::isfinite(source.elevations_m[std::size_t(other)]) ||
                source.elevations_m[std::size_t(other)] > water_level_m)
                continue;
            visited[std::size_t(other)] = 1;
            ready.push(other);
        }
    }
    return result;
}

} // namespace Aowis::Hydrology
