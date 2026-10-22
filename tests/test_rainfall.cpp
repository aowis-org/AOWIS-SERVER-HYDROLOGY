#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/rainfall.h>

#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace Aowis::Hydrology;

namespace
{
ElevationGrid grid()
{
    ElevationGrid result;
    result.columns = 4;
    result.rows = 4;
    result.geometry.origin_x = 8.0;
    result.geometry.origin_y = 50.0;
    result.geometry.pixel_width = 0.001;
    result.geometry.pixel_height = 0.001;
    result.elevations_m.assign(16, 100.0f);
    return result;
}
GeographicRing ring(double x0, double y0, double x1, double y1)
{
    return {{x0,y0},{x1,y0},{x1,y1},{x0,y1},{x0,y0}};
}
bool near(double a, double b, double tolerance = 1.0e-7)
{
    return std::abs(a-b) <= tolerance * std::max(1.0, std::abs(b));
}
bool rejected(const ElevationGrid &dem, const RainfallScenario &scenario)
{
    try { (void)calculateRainfallRunoff(dem, scenario); }
    catch (const std::invalid_argument &) { return true; }
    return false;
}
}

int main()
{
    ElevationGrid dem = grid();
    const double area = rasterRowCellAreaM2(dem.geometry, dem.columns, 1);
    RainfallScenario scenario;
    scenario.depth_mm = 25.0;
    scenario.duration_seconds = 3600.0;
    scenario.effective_runoff_fraction = 0.4;
    GeographicPolygon one;
    one.exterior = ring(8.001, 49.998, 8.002, 49.999);
    scenario.polygons = {one};
    RainfallRunoff result = calculateRainfallRunoff(dem, scenario);
    assert(result.wet_cells == 1);
    assert(near(result.covered_area_m2, area));
    assert(near(result.gross_rainfall_volume_m3, area * 0.025));
    assert(near(result.effective_runoff_volume_m3, area * 0.01));
    assert(near(result.retained_or_lost_volume_m3, area * 0.015));
    assert(near(result.average_effective_runoff_m3_s, area * 0.01 / 3600.0));
    assert(near(result.covered_fraction[5], 1.0));
    assert(!result.touches_domain_boundary);

    // A 50% split of one DEM cell is volume-weighted, not centre-selected.
    one.exterior = ring(8.001, 49.998, 8.0015, 49.999);
    scenario.polygons = {one};
    result = calculateRainfallRunoff(dem, scenario);
    assert(result.wet_cells == 1);
    assert(near(result.covered_fraction[5], 0.5));
    assert(near(result.covered_area_m2, 0.5 * area));

    // An embedded hole removes exactly half the original cell.
    one.exterior = ring(8.001, 49.998, 8.002, 49.999);
    one.holes = {ring(8.001, 49.998, 8.0015, 49.999)};
    scenario.polygons = {one};
    result = calculateRainfallRunoff(dem, scenario);
    assert(near(result.covered_fraction[5], 0.5));

    // Polygon union must not double count fully or partially overlapping
    // MultiPolygon components, even if one cuts across the other.
    one.holes.clear();
    GeographicPolygon two;
    two.exterior = ring(8.0015, 49.998, 8.002, 49.999);
    scenario.polygons = {one,two};
    result = calculateRainfallRunoff(dem, scenario);
    assert(near(result.covered_fraction[5], 1.0));
    assert(near(result.effective_runoff_volume_m3, area * 0.01));
    scenario.polygons = {two,two};
    result = calculateRainfallRunoff(dem, scenario);
    assert(near(result.covered_fraction[5], 0.5));

    // Diagonal crossing triangles test that intersection events are included
    // in the geometric-union integration (not just ring x-coordinates).
    GeographicPolygon diagonal_a;
    diagonal_a.exterior = {{8.001,49.998},{8.002,49.999},
                           {8.001,49.999},{8.001,49.998}};
    GeographicPolygon diagonal_b;
    diagonal_b.exterior = {{8.001,49.999},{8.002,49.998},
                           {8.001,49.998},{8.001,49.999}};
    scenario.polygons = {diagonal_a, diagonal_b};
    result = calculateRainfallRunoff(dem, scenario);
    assert(near(result.covered_fraction[5], 0.75));

    // The original grid border clips rain geometry. The DEM is never extended.
    one.exterior = ring(7.9995, 49.999, 8.0005, 50.001);
    scenario.polygons = {one};
    result = calculateRainfallRunoff(dem, scenario);
    assert(result.touches_domain_boundary);
    assert(result.wet_cells == 1);
    assert(near(result.covered_fraction[0], 0.5));

    dem.elevations_m[0] = std::numeric_limits<float>::quiet_NaN();
    result = calculateRainfallRunoff(dem, scenario);
    assert(result.contains_missing_dem_coverage);
    assert(result.wet_cells == 0);
    assert(result.gross_rainfall_volume_m3 == 0.0);
    dem.elevations_m[0] = 100.0f;

    // WGS84 input polygons must also work for an existing projected UTM
    // raster: the polygon is transformed into the grid's fixed UTM zone.
    ElevationGrid projected;
    projected.columns = 3;
    projected.rows = 3;
    projected.geometry.horizontal_crs = HorizontalCrs::Wgs84Utm;
    projected.geometry.utm_zone = 32;
    projected.geometry.northern_hemisphere = true;
    projected.geometry.origin_x = 499900.0;
    projected.geometry.origin_y = 5539000.0;
    projected.geometry.pixel_width = 100.0;
    projected.geometry.pixel_height = 100.0;
    projected.elevations_m.assign(9, 20.0f);
    GeographicPolygon projected_polygon;
    const MapCoordinate utm_a = utmToWgs84({32,true,500000.0,5538800.0});
    const MapCoordinate utm_b = utmToWgs84({32,true,500100.0,5538900.0});
    projected_polygon.exterior = ring(utm_a.x, utm_a.y, utm_b.x, utm_b.y);
    // Reprojected lon/lat axis-aligned rectangle covers approximately one
    // 100m UTM pixel, so cross-projection area should be close to one cell.
    scenario.polygons = {projected_polygon};
    result = calculateRainfallRunoff(projected, scenario);
    assert(result.wet_cells > 0);
    assert(result.covered_area_m2 > 0.0);

    scenario.duration_seconds = 0.0;
    assert(rejected(dem, scenario));
    scenario.duration_seconds = 3600.0;
    scenario.effective_runoff_fraction = 1.1;
    assert(rejected(dem, scenario));
    scenario.effective_runoff_fraction = 1.0;
    assert(rejected(dem, scenario) == false);
    assert(rejected(dem, RainfallScenario{}));
    bool bounded = false;
    try { (void)calculateRainfallRunoff(dem, scenario, 1); }
    catch (const std::invalid_argument &) { bounded = true; }
    assert(!bounded); // the clipped bounding box contains exactly one cell
    return 0;
}
