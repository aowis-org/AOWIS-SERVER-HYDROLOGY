#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/spill_geometry.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace Aowis::Hydrology;

namespace
{
ElevationGrid makeGrid(std::uint32_t width, std::uint32_t height,
                       const std::vector<float> &elevations)
{
    ElevationGrid grid;
    grid.columns = width;
    grid.rows = height;
    grid.geometry.origin_x = 8.0;
    grid.geometry.origin_y = 50.0;
    grid.geometry.pixel_width = 0.001;
    grid.geometry.pixel_height = 0.001;
    grid.elevations_m = elevations;
    return grid;
}

bool almost(double left, double right)
{
    return std::abs(left - right) <= 1e-8 * std::max({1.0, std::abs(left), std::abs(right)});
}
}

int main()
{
    // Three sinks joining at an isolated 12m cell. Two connections are D8
    // corners only, hence MUST NOT be assigned a fictitious face width.
    const ElevationGrid pits = makeGrid(5, 5, {
        16,16,16,16,16,
        16,10,16,10,16,
        16,16,12,16,16,
        16,16,10,16,16,
        16,16,16,16,16
    });
    const DepressionHierarchy hierarchy = buildDepressionHierarchy(pits);
    const SpillGeometry geometry = extractSpillGeometry(pits, hierarchy);
    assert(geometry.junctions.size() == 1);
    const BasinSpillJunction &junction = geometry.junctions[0];
    assert(junction.parent_basin_index == hierarchy.basins.back().index);
    assert(junction.plateau_cells == 1);
    assert(junction.saddle_cell == 12);
    assert(junction.saddle_elevation_m == 12.0);
    assert(junction.child_contacts.size() == 3);
    std::uint64_t cardinal = 0;
    std::uint64_t diagonals = 0;
    for (const BasinSpillContact &contact : junction.child_contacts)
    {
        assert(hierarchy.basins[std::size_t(contact.child_basin_index)].parent_index ==
               std::int64_t(junction.parent_basin_index));
        assert(contact.representative_saddle_cell == 12);
        assert(contact.representative_contact_wgs84.x > 8.0);
        assert(contact.representative_contact_wgs84.y < 50.0);
        assert(contact.cardinal_boundary_length_m >= 0.0);
        cardinal += contact.cardinal_contact_faces;
        diagonals += contact.diagonal_contact_pairs;
        if (contact.cardinal_contact_faces == 0)
            assert(contact.cardinal_boundary_length_m == 0.0);
    }
    assert(cardinal == 1);
    assert(diagonals == 2);

    // Simple shared-face saddle: each child has one cardinal interface.
    const ElevationGrid face = makeGrid(5, 3, {
        20,20,20,20,20,
        20, 5,10, 5,20,
        20,20,20,20,20
    });
    const DepressionHierarchy face_basins = buildDepressionHierarchy(face);
    const SpillGeometry face_geometry = extractSpillGeometry(face, face_basins);
    assert(face_geometry.junctions.size() == 1);
    assert(face_geometry.junctions[0].plateau_cells == 1);
    assert(face_geometry.junctions[0].child_contacts.size() == 2);
    for (const BasinSpillContact &contact : face_geometry.junctions[0].child_contacts)
    {
        assert(contact.cardinal_contact_faces == 1);
        assert(contact.diagonal_contact_pairs == 0);
        const double expected = rasterRowNeighborDistanceM(face.geometry, 5, 1, 0, 1);
        assert(almost(contact.cardinal_boundary_length_m, expected));
    }

    // A connected multi-cell saddle plateau must be traversed as a unit.
    const ElevationGrid broad = makeGrid(7, 5, {
        20,20,20,20,20,20,20,
        20,12,12,12,12,12,20,
        20, 5,12,12,12, 5,20,
        20,12,12,12,12,12,20,
        20,20,20,20,20,20,20
    });
    const DepressionHierarchy broad_basins = buildDepressionHierarchy(broad);
    const SpillGeometry broad_geometry = extractSpillGeometry(broad, broad_basins);
    assert(broad_geometry.junctions.size() == 1);
    assert(broad_geometry.junctions[0].plateau_cells == 13);
    for (const BasinSpillContact &contact : broad_geometry.junctions[0].child_contacts)
    {
        assert(contact.cardinal_contact_faces >= 2);
        assert(contact.cardinal_boundary_length_m > 0.0);
    }

    // Different saddle plateaus with the SAME elevation must not be merged
    // when separated by higher terrain.
    const ElevationGrid separate = makeGrid(11, 3, {
        20,20,20,20,20,20,20,20,20,20,20,
        20, 5,10, 5,20,20,20, 5,10, 5,20,
        20,20,20,20,20,20,20,20,20,20,20
    });
    const DepressionHierarchy separate_basins = buildDepressionHierarchy(separate);
    const SpillGeometry separate_geometry = extractSpillGeometry(separate, separate_basins);
    assert(separate_geometry.junctions.size() == 2);
    assert(separate_geometry.junctions[0].saddle_cell != separate_geometry.junctions[1].saddle_cell);
    assert(separate_geometry.junctions[0].plateau_cells == 1);
    assert(separate_geometry.junctions[1].plateau_cells == 1);

    // Projected input: contact coordinates remain WGS84, while contact
    // lengths are scale-adjusted metres (not raw projected units).
    ElevationGrid projected = face;
    projected.geometry.horizontal_crs = HorizontalCrs::Wgs84Utm;
    projected.geometry.utm_zone = 32;
    projected.geometry.origin_x = 500000.0;
    projected.geometry.origin_y = 5540000.0;
    projected.geometry.pixel_width = 10.0;
    projected.geometry.pixel_height = 10.0;
    const DepressionHierarchy projected_basins = buildDepressionHierarchy(projected);
    const SpillGeometry projected_geom = extractSpillGeometry(projected, projected_basins);
    assert(projected_geom.junctions.size() == 1);
    for (const BasinSpillContact &contact : projected_geom.junctions[0].child_contacts)
    {
        assert(contact.cardinal_boundary_length_m > 9.0);
        assert(contact.cardinal_boundary_length_m < 11.0);
        assert(contact.representative_contact_wgs84.x > 8.0);
        assert(contact.representative_contact_wgs84.x < 10.0);
        assert(contact.representative_contact_wgs84.y > 49.0);
        assert(contact.representative_contact_wgs84.y < 51.0);
    }

    // Varied terrain and ties: the extraction must always identify every
    // immediate merge child exactly once. All contacts are nonnegative,
    // finite and geometrically plausible. Repeated runs are deterministic.
    for (std::uint32_t seed = 1; seed <= 20; ++seed)
    {
        std::vector<float> heights(13 * 11, 40.0f);
        std::uint32_t value = seed * 747796405U;
        for (std::uint32_t y = 1; y < 10; ++y)
            for (std::uint32_t x = 1; x < 12; ++x)
            {
                value = value * 1664525U + 1013904223U;
                heights[std::size_t(y) * 13 + x] = float(5 + ((value >> 24) % 31));
            }
        const ElevationGrid random_dem = makeGrid(13, 11, heights);
        const DepressionHierarchy random_basins = buildDepressionHierarchy(random_dem);
        const SpillGeometry contacts = extractSpillGeometry(random_dem, random_basins);
        const SpillGeometry repeated = extractSpillGeometry(random_dem, random_basins);
        DepressionHierarchy reversed = random_basins;
        for (DepressionBasin &basin : reversed.basins)
            std::reverse(basin.child_indices.begin(), basin.child_indices.end());
        const SpillGeometry reordered = extractSpillGeometry(random_dem, reversed);
        assert(contacts.junctions.size() == reordered.junctions.size());
        assert(contacts.junctions.size() == repeated.junctions.size());
        if (seed == 1)
        {
            // A deterministic fixture with three bookkeeping merges at
            // 14m, physically one newly connected saddle system.
            std::uint64_t original_merges = 0;
            std::uint64_t reported_junctions = 0;
            for (const DepressionBasin &basin : random_basins.basins)
                if (basin.formation_elevation_m == 14.0 && basin.child_indices.size() >= 2)
                    ++original_merges;
            for (const BasinSpillJunction &event : contacts.junctions)
                if (event.saddle_elevation_m == 14.0)
                    ++reported_junctions;
            assert(original_merges == 3);
            assert(reported_junctions == 1);
        }
        for (std::size_t j = 0; j < contacts.junctions.size(); ++j)
        {
            const BasinSpillJunction &event = contacts.junctions[j];
            const BasinSpillJunction &again = repeated.junctions[j];
            const BasinSpillJunction &other_order = reordered.junctions[j];
            assert(event.parent_basin_index == other_order.parent_basin_index);
            assert(event.saddle_cell == other_order.saddle_cell);
            assert(event.plateau_cells == other_order.plateau_cells);
            assert(event.child_contacts.size() == other_order.child_contacts.size());
            assert(event.parent_basin_index == again.parent_basin_index);
            assert(event.saddle_cell == again.saddle_cell);
            assert(event.plateau_cells == again.plateau_cells);
            assert(event.child_contacts.size() == again.child_contacts.size());
            assert(event.child_contacts.size() >= 2);
            for (std::size_t k = 0; k < event.child_contacts.size(); ++k)
            {
                const BasinSpillContact &contact = event.child_contacts[k];
                const BasinSpillContact &repeated_contact = again.child_contacts[k];
                const BasinSpillContact &other_contact = other_order.child_contacts[k];
                assert(contact.child_basin_index == other_contact.child_basin_index);
                assert(contact.cardinal_contact_faces == other_contact.cardinal_contact_faces);
                assert(contact.diagonal_contact_pairs == other_contact.diagonal_contact_pairs);
                assert(almost(contact.cardinal_boundary_length_m,
                              other_contact.cardinal_boundary_length_m));
                assert(contact.child_basin_index == repeated_contact.child_basin_index);
                assert(contact.cardinal_contact_faces == repeated_contact.cardinal_contact_faces);
                assert(contact.diagonal_contact_pairs == repeated_contact.diagonal_contact_pairs);
                assert(almost(contact.cardinal_boundary_length_m,
                              repeated_contact.cardinal_boundary_length_m));
                assert(std::isfinite(contact.cardinal_boundary_length_m));
                assert(contact.cardinal_boundary_length_m >= 0.0);
                assert(contact.cardinal_contact_faces + contact.diagonal_contact_pairs > 0);
                std::int64_t ancestor = std::int64_t(contact.child_basin_index);
                std::size_t depth = 0;
                while (ancestor >= 0 && std::uint64_t(ancestor) != event.parent_basin_index)
                {
                    assert(++depth <= random_basins.basins.size());
                    ancestor = random_basins.basins[std::size_t(ancestor)].parent_index;
                }
                assert(ancestor == std::int64_t(event.parent_basin_index));
            }
        }
    }

    // Leaf-only terrain contains no internal merge saddle.
    const ElevationGrid single = makeGrid(3, 3, {15,15,15, 15,10,15, 15,15,15});
    const SpillGeometry no_merges = extractSpillGeometry(single, buildDepressionHierarchy(single));
    assert(no_merges.junctions.empty());

    // Boundary of the DEM is not a surveyed outlet: this API deliberately
    // extracts only internal parent/child contacts.
    bool rejected = false;
    try { (void)extractSpillGeometry(face, face_basins, 0, 100); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { (void)extractSpillGeometry(broad, broad_basins, 3, 100); }
    catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { (void)extractSpillGeometry(face, face_basins, 100, 1); }
    catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    std::atomic<bool> cancelled{true};
    rejected = false;
    try { (void)extractSpillGeometry(face, face_basins, 100, 100, &cancelled); }
    catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    ElevationGrid changed = face;
    changed.elevations_m[7] = 11.0f;
    rejected = false;
    try { (void)extractSpillGeometry(changed, face_basins); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    return 0;
}
