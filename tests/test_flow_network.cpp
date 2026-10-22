#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/flow_network.h>

#include <cassert>
#include <cmath>
#include <stdexcept>

using namespace Aowis::Hydrology;

namespace
{
ElevationGrid testGrid()
{
    ElevationGrid grid;
    grid.columns = 5;
    grid.rows = 5;
    grid.geometry.horizontal_crs = HorizontalCrs::Wgs84Geographic;
    grid.geometry.origin_x = 8.0;
    grid.geometry.origin_y = 50.0;
    grid.geometry.pixel_width = 0.001;
    grid.geometry.pixel_height = 0.001;
    grid.elevations_m.assign(25, 90.0f);
    return grid;
}
AnalysisResult junction()
{
    AnalysisResult result;
    result.source = testGrid();
    result.downstream.assign(25, -1);
    result.conditioned_elevations_m.assign(25, 90.0f);
    result.upstream_area_m2.assign(25, 0.0);
    result.downstream[6] = 12;
    result.downstream[8] = 12;
    result.downstream[12] = 17;
    result.downstream[17] = 22;
    result.conditioned_elevations_m[6] = 110.0f;
    result.conditioned_elevations_m[8] = 111.0f;
    result.conditioned_elevations_m[12] = 100.0f;
    result.conditioned_elevations_m[17] = 98.0f;
    result.conditioned_elevations_m[22] = 95.0f;
    result.upstream_area_m2[6] = 1.0e6;
    result.upstream_area_m2[8] = 1.0e6;
    result.upstream_area_m2[12] = 2.0e6;
    result.upstream_area_m2[17] = 3.0e6;
    result.upstream_area_m2[22] = 4.0e6;
    return result;
}
}

int main()
{
    const AnalysisResult result = junction();
    const FlowTrace trace = traceDownstream(result, 6, 100000.0);
    assert(trace.termination == TraceTermination::DomainOutlet);
    assert(trace.points.size() == 4);
    assert(trace.points[0].cell_index == 6);
    assert(trace.points[1].cell_index == 12);
    assert(trace.points[2].cell_index == 17);
    assert(trace.points[3].cell_index == 22);
    assert(trace.traced_length_m > 0);
    assert(trace.net_slope > 0);
    assert(trace.points[1].distance_from_start_m > 0);
    const double first_link = trace.points[1].distance_from_start_m;
    const FlowTrace short_trace = traceDownstream(result, 6, first_link * 0.9);
    assert(short_trace.termination == TraceTermination::DistanceLimit);
    assert(short_trace.points.size() == 1);
    const FlowTrace limited = traceDownstream(result, 6, 100000.0, 2);
    assert(limited.termination == TraceTermination::PointLimit);
    assert(limited.points.size() == 2);
    const FlowTrace sink = traceDownstream(result, 7, 10000.0);
    assert(sink.termination == TraceTermination::InternalSink);
    const FlowTrace reader = traceDownstream(result.source.geometry, 5, 5, 8,
        100000.0, 10000,
        [&result](std::uint64_t cell) { return result.downstream[std::size_t(cell)]; },
        [&result](std::uint64_t cell) {
            return result.conditioned_elevations_m[std::size_t(cell)];
        });
    assert(reader.points.back().cell_index == 22);

    const ChannelNetwork network = extractChannelNetwork(result, 1.0e6);
    assert(network.selected_cells == 5);
    assert(network.nodes.size() == 4); // head 6/8, confluence 12, outlet 22
    assert(network.segments.size() == 3);
    bool found_downstream = false;
    for (const ChannelSegment &segment : network.segments)
    {
        assert(segment.length_m > 0);
        assert(segment.cells.front() == segment.from_cell);
        assert(segment.cells.back() == segment.to_cell);
        if (segment.from_cell == 12)
        {
            found_downstream = true;
            assert(segment.strahler_order == 2);
            assert(segment.to_cell == 22);
            assert(segment.cells.size() == 3);
        }
        else assert(segment.strahler_order == 1);
    }
    assert(found_downstream);
    assert(extractChannelNetwork(result, 5.0e6).nodes.empty());

    bool rejected = false;
    try { (void)extractChannelNetwork(result, 1.0e6, 4); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { (void)extractChannelNetwork(result, 1.0e6, 100, 2); }
    catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);

    AnalysisResult invalid = result;
    invalid.downstream[6] = 18; // two columns away
    rejected = false;
    try { (void)traceDownstream(invalid, 6, 100000); }
    catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    invalid = result;
    invalid.downstream[22] = 17;
    rejected = false;
    try { (void)extractChannelNetwork(invalid, 1.0e6); }
    catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    return 0;
}
