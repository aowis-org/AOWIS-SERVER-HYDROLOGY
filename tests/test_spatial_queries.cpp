#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/spatial_queries.h>

#include <aowis/hydrology/analysis.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <unordered_set>
#include <vector>

using namespace Aowis::Hydrology;

namespace
{
ElevationGrid grid()
{
    ElevationGrid result;
    result.columns = 6;
    result.rows = 6;
    result.geometry.horizontal_crs = HorizontalCrs::Wgs84Geographic;
    result.geometry.origin_x = 8.0;
    result.geometry.origin_y = 50.0;
    result.geometry.pixel_width = 0.001;
    result.geometry.pixel_height = 0.001;
    return result;
}
GeographicRing ring(double x0, double y0, double x1, double y1)
{
    return {{x0,y0},{x1,y0},{x1,y1},{x0,y1},{x0,y0}};
}
}

int main()
{
    const ElevationGrid surface = grid();
    std::vector<std::uint8_t> mask(36, 0);
    mask[14] = mask[15] = mask[20] = mask[21] = 1;
    const WatershedBoundary square = polygonizeCatchment(surface, mask);
    assert(square.polygons.size() == 1);
    assert(square.polygons[0].rings.size() == 1);
    assert(square.polygons[0].rings[0].size() == 5);
    assert(square.contributing_cells == 4);
    assert(!square.touches_domain_boundary);
    const double reference_area = rasterRowCellAreaM2(surface.geometry, 6, 2) * 2 +
                                  rasterRowCellAreaM2(surface.geometry, 6, 3) * 2;
    assert(std::abs(square.contributing_area_m2 - reference_area) < 0.01);
    const MapCoordinate upper_left = gridVertexWgs84(surface.geometry, square.polygons[0].rings[0][0]);
    assert(upper_left.x >= 8.0 && upper_left.x <= 8.006);
    assert(upper_left.y <= 50.0 && upper_left.y >= 49.994);

    // Diagonal connection is not a shared boundary. It must be represented as
    // two distinct polygons instead of a self-touching invalid ring.
    mask.assign(36, 0);
    mask[7] = 1;
    mask[14] = 1;
    const WatershedBoundary diagonal = polygonizeCatchment(surface, mask);
    assert(diagonal.polygons.size() == 2);

    // A 3x3 collection of cells with the central cell empty has one hole.
    mask.assign(36, 0);
    for (int y = 1; y <= 3; ++y)
        for (int x = 1; x <= 3; ++x)
            mask[std::size_t(y * 6 + x)] = 1;
    mask[14] = 0;
    const WatershedBoundary hole = polygonizeCatchment(surface, mask);
    assert(hole.polygons.size() == 1);
    assert(hole.polygons[0].rings.size() == 2);
    assert(hole.contributing_cells == 8);

    mask.assign(36, 0);
    mask[0] = 1;
    assert(polygonizeCatchment(surface, mask).touches_domain_boundary);
    bool rejected = false;
    try { (void)polygonizeCatchment(surface, mask, 3); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { (void)polygonizeCatchment(surface, mask, 4); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(!rejected);

    // Deterministic raster-mask stress test: corner contacts, holes, and
    // fragmented components must always close into valid, nonempty rings.
    std::uint32_t random_state = 0x12345678U;
    for (int trial = 0; trial < 256; ++trial)
    {
        mask.assign(36, 0);
        std::uint64_t expected = 0;
        for (std::uint8_t &value : mask)
        {
            random_state = random_state * 1664525U + 1013904223U;
            value = (random_state >> 28) % 3 == 0 ? 1 : 0;
            expected += value;
        }
        if (expected == 0) { mask[0] = 1; expected = 1; }
        const WatershedBoundary random_boundary = polygonizeCatchment(surface, mask);
        assert(random_boundary.contributing_cells == expected);
        assert(!random_boundary.polygons.empty());
        for (const WatershedPolygon &polygon : random_boundary.polygons)
            for (const std::vector<GridVertex> &ring : polygon.rings)
            {
                assert(ring.size() >= 5);
                assert(ring.front().x == ring.back().x);
                assert(ring.front().y == ring.back().y);
            }
    }

    // Polygon source at cell (1,1), with a downstream trunk at (2,2) ->
    // (3,3) -> (4,4) -> (5,5). The complete-domain accumulation values are
    // retained when the induced channel network is extracted.
    AnalysisResult result;
    result.source = surface;
    result.downstream.assign(36, -1);
    result.upstream_area_m2.assign(36, 0.0);
    result.downstream[7] = 14;
    result.downstream[14] = 21;
    result.downstream[21] = 28;
    result.downstream[28] = 35;
    result.upstream_area_m2[7] = 100000.0;
    result.upstream_area_m2[14] = 200000.0;
    result.upstream_area_m2[21] = 300000.0;
    result.upstream_area_m2[28] = 400000.0;
    result.upstream_area_m2[35] = 500000.0;
    GeographicPolygon source;
    source.exterior = ring(8.001, 49.998, 8.002, 49.999);
    const PolygonDrainageResult downstream = downstreamNetworkFromPolygon(
        result, {source}, 100000.0);
    assert(downstream.source_cells == 1);
    assert(downstream.downstream_affected_cells == 5);
    assert(downstream.affected_boundary_cells == 1);
    assert(downstream.network.selected_cells == 5);
    assert(downstream.network.segments.size() == 1);
    assert(downstream.network.segments[0].to_cell == 35);
    assert(!downstream.clipped_to_window);

    const DrainageWindow window{8.0, 49.997, 8.004, 50.0};
    const PolygonDrainageResult clipped = downstreamNetworkFromPolygon(
        result, {source}, 100000.0, window);
    assert(clipped.source_cells == 1);
    assert(clipped.clipped_to_window);
    assert(clipped.network.selected_cells < downstream.network.selected_cells);

    const ChannelNetwork windowed = extractWindowChannelNetwork(result, 100000.0, window);
    assert(windowed.selected_cells < downstream.network.selected_cells);
    assert(windowed.selected_cells > 0);

    // Holes remove contributing cell centres from the geographic selection.
    GeographicPolygon excluded;
    excluded.exterior = ring(8.0, 49.994, 8.006, 50.0);
    excluded.holes.push_back(ring(8.001, 49.998, 8.002, 49.999));
    const PolygonDrainageResult removed = downstreamNetworkFromPolygon(result, {excluded}, 100000.0);
    assert(removed.source_cells == 4); // remaining positive-area cells only

    rejected = false;
    try { (void)downstreamNetworkFromPolygon(result, {source}, 100000.0, {}, 0); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { (void)downstreamNetworkFromPolygon(result, {source}, 100000.0, {}, 10, 3); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    GeographicPolygon malformed = source;
    malformed.exterior.pop_back();
    rejected = false;
    try { (void)downstreamNetworkFromPolygon(result, {malformed}, 100000.0); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);

    // Reader-based queries on a larger synthetic terrain: identical results
    // to the in-memory overloads while reading only the cells they need.
    {
        ElevationGrid terrain;
        terrain.columns = 120;
        terrain.rows = 90;
        terrain.geometry = grid().geometry;
        terrain.geometry.pixel_width = 0.0003;
        terrain.geometry.pixel_height = 0.0003;
        terrain.elevations_m.resize(std::size_t(terrain.columns) * terrain.rows);
        for (std::size_t i = 0; i < terrain.elevations_m.size(); ++i)
        {
            const double x = double(i % terrain.columns);
            const double y = double(i / terrain.columns);
            terrain.elevations_m[i] = float(200.0 - 0.5 * x - 0.3 * y +
                4.0 * std::sin(0.21 * x) * std::cos(0.17 * y));
        }
        const AnalysisResult flow = analyze(terrain);
        const std::uint64_t cells = std::uint64_t(terrain.columns) * terrain.rows;
        std::uint64_t link_reads = 0;
        std::uint64_t area_reads = 0;
        std::unordered_set<std::uint64_t> area_cells;
        const DownstreamReader links = [&](std::uint64_t cell)
        {
            assert(cell < cells);
            ++link_reads;
            return flow.downstream[std::size_t(cell)];
        };
        const UpstreamAreaReader areas = [&](std::uint64_t cell)
        {
            assert(cell < cells);
            ++area_reads;
            area_cells.insert(cell);
            return flow.upstream_area_m2[std::size_t(cell)];
        };
        const auto same = [](const ChannelNetwork &a, const ChannelNetwork &b)
        {
            if (a.selected_cells != b.selected_cells || a.nodes.size() != b.nodes.size() ||
                a.segments.size() != b.segments.size())
                return false;
            for (std::size_t i = 0; i < a.segments.size(); ++i)
                if (a.segments[i].cells != b.segments[i].cells ||
                    a.segments[i].strahler_order != b.segments[i].strahler_order ||
                    a.segments[i].length_m != b.segments[i].length_m)
                    return false;
            return true;
        };
        const double threshold = 400000.0;
        const ChannelNetwork full = extractChannelNetwork(terrain.geometry, terrain.columns,
            terrain.rows, links, areas, threshold, 25000, 2000);
        assert(full.selected_cells > 0 && !full.segments.empty());
        assert(same(full, extractChannelNetwork(flow, threshold, 25000, 2000)));
        // One sequential pass over the area raster; links only for selected cells.
        assert(area_reads == cells);
        assert(link_reads == full.selected_cells);

        const DrainageWindow window{8.006, 50.0 - 0.02, 8.024, 50.0 - 0.004};
        link_reads = 0;
        const ChannelNetwork clipped = extractWindowChannelNetwork(terrain.geometry,
            terrain.columns, terrain.rows, links, areas, threshold, window);
        assert(same(clipped, extractWindowChannelNetwork(flow, threshold, window)));
        assert(link_reads == clipped.selected_cells);

        // The polygon query reads areas only around the polygon and along
        // the traced downstream paths, never the whole raster.
        GeographicPolygon square;
        square.exterior = ring(8.009, 49.991, 8.012, 49.994);
        link_reads = 0;
        area_reads = 0;
        area_cells.clear();
        const PolygonDrainageResult affected = downstreamNetworkFromPolygon(terrain.geometry,
            terrain.columns, terrain.rows, links, areas, {square}, threshold);
        const PolygonDrainageResult dense = downstreamNetworkFromPolygon(flow, {square}, threshold);
        assert(affected.source_cells == dense.source_cells && affected.source_cells > 0);
        assert(affected.downstream_affected_cells == dense.downstream_affected_cells);
        assert(affected.affected_boundary_cells == dense.affected_boundary_cells);
        assert(same(affected.network, dense.network));
        assert(area_cells.size() < cells / 10);
        assert(link_reads <= affected.downstream_affected_cells + affected.network.selected_cells);
    }
    return 0;
}
