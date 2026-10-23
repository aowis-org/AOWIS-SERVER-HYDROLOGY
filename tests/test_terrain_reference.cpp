#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/analysis.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace Aowis::Hydrology;

namespace
{
ElevationGrid projected(std::uint32_t columns, std::uint32_t rows)
{
    ElevationGrid grid;
    grid.columns = columns;
    grid.rows = rows;
    grid.geometry.horizontal_crs = HorizontalCrs::Wgs84Utm;
    grid.geometry.vertical_datum = VerticalDatum::Egm2008;
    grid.geometry.utm_zone = 32;
    grid.geometry.northern_hemisphere = true;
    grid.geometry.origin_x = 500000.0;
    grid.geometry.origin_y = 5600000.0;
    grid.geometry.pixel_width = 30.0;
    grid.geometry.pixel_height = 30.0;
    grid.elevations_m.resize(std::size_t(columns) * rows);
    return grid;
}
}

int main()
{
    // Independently constructed plane z(x,y)=200-2*x: physical east-facing
    // drop is steeper than the diagonal alternative, so every cell must
    // drain eastward inside its own row. Each east-edge cell drains a
    // distinct row; its catchment has exactly the known 8 cells.
    ElevationGrid plane = projected(8, 6);
    for (std::uint32_t r = 0; r < plane.rows; ++r)
        for (std::uint32_t c = 0; c < plane.columns; ++c)
            plane.elevations_m[std::size_t(r)*plane.columns+c] = float(200-2*c);
    const AnalysisResult flow = analyze(plane);
    assert(flow.conditioned_cell_count == 0);
    assert(flow.potential_fill_cell_count == 0);
    for (std::uint32_t r = 0; r < plane.rows; ++r)
        for (std::uint32_t c = 0; c < plane.columns; ++c)
        {
            const std::uint64_t cell = std::uint64_t(r)*plane.columns+c;
            assert(flow.downstream[cell] == (c == 7 ? -1 : std::int64_t(cell+1)));
            assert(flow.upstream_cell_count[cell] == c+1);
            assert(flow.potential_fill_depth_m[cell] == 0.0f);
        }
    for (std::uint32_t r = 0; r < plane.rows; ++r)
    {
        const std::uint64_t outlet = std::uint64_t(r)*plane.columns+7;
        const auto mask = delineateCatchment(flow, outlet);
        std::uint32_t count = 0;
        for (std::uint32_t i = 0; i < mask.size(); ++i)
        {
            const bool expected = i / plane.columns == r;
            assert(bool(mask[i]) == expected);
            count += mask[i];
        }
        assert(count == 8);
        // Projected UTM area is close to 8*30*30 m2; the system applies
        // local geodesic scale corrections that are deliberately not copied
        // into the independent nominal area reference.
        assert(std::abs(flow.upstream_area_m2[outlet]-7200.0) < 35.0);
    }

    // Independent prismatic depression: 3x3 cells at z=0 surrounded by a
    // z=10 closed rim. The potential fill is 9*10m*900m2 ~81000m3.
    // This is a DEM volume identity, NOT a flood-event volume prediction.
    ElevationGrid pit = projected(5, 5);
    pit.elevations_m.assign(25, 10.0f);
    for (int r = 1; r <= 3; ++r)
        for (int c = 1; c <= 3; ++c)
            pit.elevations_m[std::size_t(r)*5+c] = 0.0f;
    const AnalysisResult depressed = analyze(pit);
    assert(depressed.potential_fill_cell_count == 9);
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 5; ++c)
        {
            const auto i = std::size_t(r)*5+c;
            assert(depressed.potential_fill_depth_m[i] ==
                ((r >= 1 && r <= 3 && c >= 1 && c <= 3) ? 10.0f : 0.0f));
        }
    assert(std::abs(depressed.potential_fill_volume_m3-81000.0) < 400.0);
}
