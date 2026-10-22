#include <aowis/hydrology/spill_geometry.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace Aowis::Hydrology
{
namespace
{
constexpr int Dx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int Dy[8] = {0, 1, 1, 1, 0, -1, -1, -1};

void checkCancellation(const std::atomic<bool> *requested)
{
    if (requested != nullptr && requested->load(std::memory_order_relaxed))
        throw std::runtime_error("Spill geometry cancelled");
}

bool neighbor(const ElevationGrid &grid, std::uint64_t cell, int direction,
              std::uint64_t *next)
{
    const std::int64_t x = std::int64_t(cell % grid.columns) + Dx[direction];
    const std::int64_t y = std::int64_t(cell / grid.columns) + Dy[direction];
    if (x < 0 || y < 0 || x >= grid.columns || y >= grid.rows)
        return false;
    *next = std::uint64_t(y) * grid.columns + std::uint64_t(x);
    return true;
}

struct Components
{
    explicit Components(std::size_t count)
        : parents(count, -1), sizes(count, 0) {}

    std::vector<std::int64_t> parents;
    std::vector<std::uint64_t> sizes;

    std::uint64_t find(std::uint64_t cell)
    {
        std::uint64_t root = cell;
        while (this->parents[std::size_t(root)] != std::int64_t(root))
            root = std::uint64_t(this->parents[std::size_t(root)]);
        while (cell != root)
        {
            const std::uint64_t next = std::uint64_t(this->parents[std::size_t(cell)]);
            this->parents[std::size_t(cell)] = std::int64_t(root);
            cell = next;
        }
        return root;
    }

    void join(std::uint64_t left, std::uint64_t right)
    {
        left = find(left);
        right = find(right);
        if (left == right) return;
        if (this->sizes[std::size_t(left)] < this->sizes[std::size_t(right)] ||
            (this->sizes[std::size_t(left)] == this->sizes[std::size_t(right)] && right < left))
            std::swap(left, right);
        this->parents[std::size_t(right)] = std::int64_t(left);
        this->sizes[std::size_t(left)] += this->sizes[std::size_t(right)];
    }
};

struct BoundaryPair
{
    std::uint64_t plateau_cell = 0;
    std::uint64_t lower_cell = 0;
    std::uint64_t lower_root = 0; // DSU root BEFORE the height group activates
    int direction = 0;
};

struct PreparedJunction
{
    BasinSpillJunction junction;
    std::unordered_map<std::uint64_t, std::size_t> by_lower_root;
};

// Hierarchy builders may produce multiple nested parent nodes at EXACTLY the
// same height. None is an independently resolved hydraulic opening. Flatten
// their zero-height intermediate merges to identify the pre-existing, lower
// connected components that actually touch the newly flooded saddle.
PreparedJunction prepareJunction(
    const ElevationGrid &dem, const DepressionHierarchy &hierarchy,
    const DepressionBasin &parent, Components &lower)
{
    const std::uint64_t count = dem.elevations_m.size();
    PreparedJunction prepared;
    prepared.junction.parent_basin_index = parent.index;
    prepared.junction.saddle_elevation_m = parent.formation_elevation_m;
    if (parent.child_indices.empty() ||
        parent.child_indices.front() >= hierarchy.basins.size())
        throw std::invalid_argument("Invalid parent merge in spill geometry");
    // The parent's spill_cell is its later OUTLET, not its formation saddle.
    // Direct children all record the formation saddle as their spill_cell.
    const std::uint64_t seed = hierarchy.basins[
        std::size_t(parent.child_indices.front())].spill_cell;
    if (seed >= count || double(dem.elevations_m[std::size_t(seed)]) !=
        parent.formation_elevation_m)
        throw std::invalid_argument("Merge saddle elevation disagrees with the source DEM");
    prepared.junction.saddle_cell = seed;

    for (const std::uint64_t child_id : parent.child_indices)
    {
        if (child_id >= hierarchy.basins.size() ||
            hierarchy.basins[std::size_t(child_id)].parent_index != std::int64_t(parent.index) ||
            hierarchy.basins[std::size_t(child_id)].spill_cell != seed)
            throw std::invalid_argument("Invalid parent-child saddle relationship");
        std::vector<std::uint64_t> pending{child_id};
        std::size_t visited_nodes = 0;
        while (!pending.empty())
        {
            if (++visited_nodes > hierarchy.basins.size())
                throw std::invalid_argument("Cyclic or duplicated basin descendants");
            const std::uint64_t id = pending.back();
            pending.pop_back();
            if (id >= hierarchy.basins.size() || id >= parent.index)
                throw std::invalid_argument("Invalid or cyclic basin hierarchy");
            const DepressionBasin &node = hierarchy.basins[std::size_t(id)];
            if (node.formation_elevation_m == parent.formation_elevation_m &&
                !node.child_indices.empty())
            {
                for (const std::uint64_t descendant : node.child_indices)
                    pending.push_back(descendant);
                continue;
            }
            // A newly formed, dry plateau minimum at this exact elevation
            // has no pre-existing storage or lower-cell contact.
            if (node.minimum_elevation_m >= parent.formation_elevation_m)
                continue;
            if (node.minimum_cell >= count ||
                double(dem.elevations_m[std::size_t(node.minimum_cell)]) !=
                    node.minimum_elevation_m ||
                lower.parents[std::size_t(node.minimum_cell)] < 0)
                throw std::invalid_argument("Spill lower basin does not match source DEM");
            const std::uint64_t root = lower.find(node.minimum_cell);
            BasinSpillContact contact;
            contact.child_basin_index = id;
            prepared.junction.child_contacts.push_back(contact);
            if (!prepared.by_lower_root.emplace(root,
                    prepared.junction.child_contacts.size() - 1).second)
                throw std::invalid_argument("Duplicate below-saddle connected component");
        }
    }
    return prepared;
}

void addContact(const ElevationGrid &dem, const BoundaryPair &pair,
                BasinSpillContact &contact)
{
    if (contact.cardinal_contact_faces == 0 && contact.diagonal_contact_pairs == 0)
    {
        contact.representative_saddle_cell = pair.plateau_cell;
        contact.representative_child_cell = pair.lower_cell;
        const MapCoordinate a = pixelCentreWgs84(dem.geometry,
            std::uint32_t(pair.plateau_cell % dem.columns),
            std::uint32_t(pair.plateau_cell / dem.columns));
        const MapCoordinate b = pixelCentreWgs84(dem.geometry,
            std::uint32_t(pair.lower_cell % dem.columns),
            std::uint32_t(pair.lower_cell / dem.columns));
        contact.representative_contact_wgs84 = {(a.x + b.x) / 2.0,
                                                (a.y + b.y) / 2.0};
    }
    if (Dx[pair.direction] != 0 && Dy[pair.direction] != 0)
        ++contact.diagonal_contact_pairs;
    else
    {
        ++contact.cardinal_contact_faces;
        // Raster-boundary segment length, not hydraulic weir width.
        contact.cardinal_boundary_length_m += rasterRowNeighborDistanceM(
            dem.geometry, dem.columns,
            std::uint32_t(pair.plateau_cell / dem.columns),
            Dx[pair.direction] == 0 ? 1 : 0,
            Dy[pair.direction] == 0 ? 1 : 0);
    }
}
} // namespace

SpillGeometry extractSpillGeometry(
    const ElevationGrid &original_dem, const DepressionHierarchy &hierarchy,
    std::uint64_t max_plateau_cells, std::uint64_t max_contact_pairs,
    const std::atomic<bool> *cancel_requested)
{
    checkCancellation(cancel_requested);
    const std::uint64_t count = std::uint64_t(original_dem.columns) * original_dem.rows;
    if (original_dem.columns < 2 || original_dem.rows < 2 ||
        original_dem.elevations_m.size() != count ||
        hierarchy.columns != original_dem.columns ||
        hierarchy.rows != original_dem.rows ||
        max_plateau_cells == 0 || max_contact_pairs == 0)
        throw std::invalid_argument("Invalid raster or limits for spill geometry");
    validateRasterGeometry(original_dem.geometry, original_dem.columns, original_dem.rows);

    std::vector<std::uint64_t> sorted;
    sorted.reserve(std::size_t(count));
    for (std::uint64_t cell = 0; cell < count; ++cell)
    {
        if ((cell & 4095U) == 0) checkCancellation(cancel_requested);
        if (std::isfinite(original_dem.elevations_m[std::size_t(cell)]))
            sorted.push_back(cell);
    }
    if (sorted.size() != hierarchy.valid_cells)
        throw std::invalid_argument("Source DEM coverage does not match basin hierarchy");
    std::sort(sorted.begin(), sorted.end(), [&original_dem](std::uint64_t left, std::uint64_t right)
    {
        const float a = original_dem.elevations_m[std::size_t(left)];
        const float b = original_dem.elevations_m[std::size_t(right)];
        return a == b ? left < right : a < b;
    });

    std::vector<std::uint64_t> merges;
    merges.reserve(hierarchy.basins.size());
    for (std::size_t index = 0; index < hierarchy.basins.size(); ++index)
    {
        const DepressionBasin &basin = hierarchy.basins[index];
        if (basin.index != index)
            throw std::invalid_argument("Invalid basin hierarchy index");
        if (basin.child_indices.size() < 2) continue;
        if (basin.parent_index >= 0 &&
            std::uint64_t(basin.parent_index) < hierarchy.basins.size() &&
            hierarchy.basins[std::size_t(basin.parent_index)].formation_elevation_m ==
                basin.formation_elevation_m)
            continue; // Intermediate bookkeeping node at the SAME saddle.
        merges.push_back(index);
    }
    std::sort(merges.begin(), merges.end(), [&hierarchy](std::uint64_t left, std::uint64_t right)
    {
        const DepressionBasin &a = hierarchy.basins[std::size_t(left)];
        const DepressionBasin &b = hierarchy.basins[std::size_t(right)];
        return a.formation_elevation_m == b.formation_elevation_m ?
            left < right : a.formation_elevation_m < b.formation_elevation_m;
    });

    Components components{std::size_t(count)};
    SpillGeometry result;
    result.junctions.reserve(merges.size());
    std::size_t first = 0;
    std::size_t next_merge = 0;
    while (first < sorted.size())
    {
        checkCancellation(cancel_requested);
        const float height = original_dem.elevations_m[std::size_t(sorted[first])];
        std::size_t end = first + 1;
        while (end < sorted.size() && original_dem.elevations_m[std::size_t(sorted[end])] == height)
            ++end;
        std::vector<PreparedJunction> prepared;
        std::vector<BoundaryPair> pairs;
        if (next_merge < merges.size() &&
            hierarchy.basins[std::size_t(merges[next_merge])].formation_elevation_m == double(height))
        {
            while (next_merge < merges.size() &&
                hierarchy.basins[std::size_t(merges[next_merge])].formation_elevation_m == double(height))
            {
                prepared.push_back(prepareJunction(original_dem, hierarchy,
                    hierarchy.basins[std::size_t(merges[next_merge])], components));
                ++next_merge;
            }
            // Snapshot every lower connected-component root BEFORE the new
            // plateau activates. Its root identifiers would otherwise merge.
            for (std::size_t at = first; at < end; ++at)
            {
                const std::uint64_t cell = sorted[at];
                for (int direction = 0; direction < 8; ++direction)
                {
                    std::uint64_t other = 0;
                    if (neighbor(original_dem, cell, direction, &other) &&
                        components.parents[std::size_t(other)] >= 0)
                    {
                        if (pairs.size() >= max_contact_pairs)
                            throw std::runtime_error("Spill contact pair limit exceeded");
                        pairs.push_back({cell, other, components.find(other), direction});
                    }
                }
            }
        }
        // Flood all equal-height cells together; after unions, two plateau
        // pieces connected THROUGH a lower component belong to ONE junction.
        for (std::size_t at = first; at < end; ++at)
        {
            const std::uint64_t cell = sorted[at];
            components.parents[std::size_t(cell)] = std::int64_t(cell);
            components.sizes[std::size_t(cell)] = 1;
        }
        for (std::size_t at = first; at < end; ++at)
        {
            const std::uint64_t cell = sorted[at];
            for (int direction = 0; direction < 8; ++direction)
            {
                std::uint64_t other = 0;
                if (neighbor(original_dem, cell, direction, &other) &&
                    components.parents[std::size_t(other)] >= 0)
                    components.join(cell, other);
            }
        }
        if (!prepared.empty())
        {
            std::unordered_map<std::uint64_t, std::size_t> by_flood_root;
            for (std::size_t i = 0; i < prepared.size(); ++i)
            {
                const std::uint64_t root = components.find(prepared[i].junction.saddle_cell);
                if (!by_flood_root.emplace(root, i).second)
                    throw std::invalid_argument("Unmerged parent nodes share one flooded saddle");
            }
            for (std::size_t at = first; at < end; ++at)
            {
                const std::uint64_t root = components.find(sorted[at]);
                const std::unordered_map<std::uint64_t, std::size_t>::const_iterator target =
                    by_flood_root.find(root);
                if (target == by_flood_root.end()) continue;
                if (max_plateau_cells == 0)
                    throw std::runtime_error("Spill plateau cell limit exceeded");
                --max_plateau_cells;
                ++prepared[target->second].junction.plateau_cells;
            }
            for (const BoundaryPair &pair : pairs)
            {
                const std::uint64_t root = components.find(pair.plateau_cell);
                const std::unordered_map<std::uint64_t, std::size_t>::const_iterator target =
                    by_flood_root.find(root);
                if (target == by_flood_root.end()) continue;
                if (max_contact_pairs == 0)
                    throw std::runtime_error("Spill contact pair limit exceeded");
                --max_contact_pairs;
                PreparedJunction &junction = prepared[target->second];
                const std::unordered_map<std::uint64_t, std::size_t>::const_iterator child =
                    junction.by_lower_root.find(pair.lower_root);
                if (child == junction.by_lower_root.end())
                    throw std::invalid_argument("Flooded saddle touches an unregistered lower component");
                addContact(original_dem, pair, junction.junction.child_contacts[child->second]);
            }
            for (PreparedJunction &event : prepared)
            {
                for (const BasinSpillContact &contact : event.junction.child_contacts)
                    if (contact.cardinal_contact_faces + contact.diagonal_contact_pairs == 0)
                        throw std::invalid_argument("A below-saddle component has no saddle contact");
                std::sort(event.junction.child_contacts.begin(), event.junction.child_contacts.end(),
                    [](const BasinSpillContact &a, const BasinSpillContact &b)
                    { return a.child_basin_index < b.child_basin_index; });
                result.junctions.push_back(std::move(event.junction));
            }
        }
        first = end;
    }
    if (next_merge != merges.size())
        throw std::invalid_argument("Merge saddle elevation not found in source DEM");
    std::sort(result.junctions.begin(), result.junctions.end(),
        [](const BasinSpillJunction &a, const BasinSpillJunction &b)
        { return a.parent_basin_index < b.parent_basin_index; });
    return result;
}

} // namespace Aowis::Hydrology
