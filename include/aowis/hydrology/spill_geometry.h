#ifndef AOWIS_HYDROLOGY_SPILL_GEOMETRY_H
#define AOWIS_HYDROLOGY_SPILL_GEOMETRY_H

#include <aowis/hydrology/depression_hierarchy.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace Aowis::Hydrology
{

// DEM-derived adjacency between a merge-saddle and ONE lower connected
// component. Equal-elevation nested merges are collapsed, so the component's
// basin index can be a DESCENDANT rather than an immediate parent child. A cardinal contact shares a raster cell face; a diagonal contact
// meets at a corner and has ZERO physical face width in this representation.
// Counts/lengths describe a raster boundary, NOT an effective weir opening.
struct BasinSpillContact
{
    std::uint64_t child_basin_index = 0;
    std::uint64_t representative_saddle_cell = 0;
    std::uint64_t representative_child_cell = 0;
    MapCoordinate representative_contact_wgs84;
    std::uint64_t cardinal_contact_faces = 0;
    std::uint64_t diagonal_contact_pairs = 0;
    double cardinal_boundary_length_m = 0.0;
};

// A connected flooded junction where an internal parent joins its children.
// Plateau cells may connect through previously flooded lower components.
// Equal-elevation nested merge nodes are represented only by their topmost
// parent, preventing duplicate counting of one physical junction.
struct BasinSpillJunction
{
    std::uint64_t parent_basin_index = 0;
    double saddle_elevation_m = 0.0;
    std::uint64_t saddle_cell = 0;
    std::uint64_t plateau_cells = 0;
    std::vector<BasinSpillContact> child_contacts;
};

struct SpillGeometry
{
    std::vector<BasinSpillJunction> junctions;
};

// Reconstruct internal merge-saddle contacts directly from the ORIGINAL DEM
// and a hierarchy produced from it. Uses a single elevation-ordered sweep:
// connected components below each saddle are identified before its plateau
// is activated. Collapses equal-height parent/child chains to their common
// flooded junction, whose contacts identify pre-existing lower components. Does not modify/persist the hierarchy, redistribution or
// inundation caches. Root-basin exits to the domain edge are NOT described.
//
// max_plateau_cells bounds the cumulative number of visited saddle pixels;
// max_contact_pairs bounds cardinal+diagonal DEM neighbour encounters.
// Throws on invalid/mismatched inputs, budget exhaustion or cancellation.
SpillGeometry extractSpillGeometry(
    const ElevationGrid &original_dem,
    const DepressionHierarchy &hierarchy,
    std::uint64_t max_plateau_cells = 2000000,
    std::uint64_t max_contact_pairs = 16000000,
    const std::atomic<bool> *cancel_requested = nullptr);

} // namespace Aowis::Hydrology
#endif
