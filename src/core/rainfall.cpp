#include <aowis/hydrology/rainfall.h>

#include <GeographicLib/UTMUPS.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Aowis::Hydrology
{
namespace
{
struct Point
{
    double x = 0.0;
    double y = 0.0;
};
using Ring = std::vector<Point>;
using Interval = std::pair<double, double>;

struct Polygon
{
    Ring exterior;
    std::vector<Ring> holes;
    double west = 0.0;
    double east = 0.0;
    double north = 0.0;
    double south = 0.0;
};

struct Edge
{
    Point a;
    Point b;
};

Point toPixel(const RasterGeometry &geometry, const MapCoordinate &point)
{
    double x = point.x;
    double y = point.y;
    if (geometry.horizontal_crs == HorizontalCrs::Wgs84Utm)
    {
        int zone = 0;
        bool north = false;
        GeographicLib::UTMUPS::Forward(point.y, point.x, zone, north, x, y,
                                       geometry.utm_zone);
        if (zone != geometry.utm_zone || north != geometry.northern_hemisphere)
            throw std::invalid_argument("Rainfall polygon cannot be projected into the raster UTM hemisphere");
    }
    return {(x - geometry.origin_x) / geometry.pixel_width,
            (geometry.origin_y - y) / geometry.pixel_height};
}

Ring projectRing(const RasterGeometry &geometry, const GeographicRing &ring,
                 std::uint64_t *total_vertices)
{
    if (ring.size() < 4 || ring.size() > 2048 ||
        *total_vertices + ring.size() > 30000 ||
        ring.front().x != ring.back().x || ring.front().y != ring.back().y)
        throw std::invalid_argument("Invalid rainfall polygon ring or vertex limit");
    Ring projected;
    projected.reserve(ring.size() - 1);
    for (std::size_t i = 0; i + 1 < ring.size(); ++i)
    {
        const MapCoordinate &point = ring[i];
        if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
            point.x < -180.0 || point.x > 180.0 ||
            point.y < (geometry.horizontal_crs == HorizontalCrs::Wgs84Utm ? -80.0 : -85.0) ||
            point.y > (geometry.horizontal_crs == HorizontalCrs::Wgs84Utm ? 84.0 : 85.0))
            throw std::invalid_argument("Invalid rainfall polygon WGS84 coordinate");
        if (i > 0 && std::abs(point.x - ring[i-1].x) > 180.0)
            throw std::invalid_argument("Antimeridian-crossing polygons are not supported");
        projected.push_back(toPixel(geometry, point));
    }
    if (std::abs(ring.back().x - ring[ring.size()-2].x) > 180.0)
        throw std::invalid_argument("Antimeridian-crossing polygons are not supported");
    long double double_area = 0;
    for (std::size_t i = 0; i < projected.size(); ++i)
    {
        const Point &a = projected[i];
        const Point &b = projected[(i+1) % projected.size()];
        double_area += static_cast<long double>(a.x) * b.y -
                       static_cast<long double>(b.x) * a.y;
    }
    if (!std::isfinite(double(double_area)) || std::abs(double(double_area)) < 1.0e-15)
        throw std::invalid_argument("Invalid zero-area rainfall polygon ring");
    *total_vertices += ring.size();
    return projected;
}

// Sutherland-Hodgman clipping of one polygon ring to one unit cell.
Ring clipEdge(const Ring &source, int axis, double limit, bool keep_greater)
{
    Ring output;
    if (source.empty()) return output;
    const auto inside = [axis, limit, keep_greater](const Point &point)
    {
        const double coordinate = axis == 0 ? point.x : point.y;
        return keep_greater ? coordinate >= limit : coordinate <= limit;
    };
    const auto intersection = [axis, limit](const Point &a, const Point &b)
    {
        const double first = axis == 0 ? a.x : a.y;
        const double second = axis == 0 ? b.x : b.y;
        const double t = (limit - first) / (second - first);
        Point point{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)};
        if (axis == 0) point.x = limit;
        else point.y = limit;
        return point;
    };
    Point previous = source.back();
    bool previous_inside = inside(previous);
    for (const Point &point : source)
    {
        const bool current_inside = inside(point);
        if (current_inside != previous_inside)
            output.push_back(intersection(previous, point));
        if (current_inside) output.push_back(point);
        previous = point;
        previous_inside = current_inside;
    }
    return output;
}

// Row strip [y, y+1] first, then the cell column [x, x+1]. The intersection
// of convex half-planes is order independent; clipping each ring to its row
// strip once per row reduces the per-cell clipping work from all ring
// vertices to the few vertices inside that strip.
Ring clipToRow(const Ring &ring, double y)
{
    return clipEdge(clipEdge(ring, 1, y, true), 1, y + 1.0, false);
}

Ring clipToColumn(const Ring &ring, double x)
{
    return clipEdge(clipEdge(ring, 0, x, true), 0, x + 1.0, false);
}

struct RowPolygon
{
    Ring exterior;
    std::vector<Ring> holes;
    double west = 0.0;
    double east = 0.0;
};

std::vector<RowPolygon> rowPolygons(const std::vector<Polygon> &polygons, double y)
{
    std::vector<RowPolygon> result;
    for (const Polygon &polygon : polygons)
    {
        if (polygon.south <= y || polygon.north >= y + 1.0) continue;
        RowPolygon strip;
        strip.exterior = clipToRow(polygon.exterior, y);
        if (strip.exterior.size() < 3) continue;
        strip.west = strip.east = strip.exterior.front().x;
        for (const Point &point : strip.exterior)
        {
            strip.west = std::min(strip.west, point.x);
            strip.east = std::max(strip.east, point.x);
        }
        for (const Ring &hole : polygon.holes)
        {
            Ring cutout = clipToRow(hole, y);
            if (cutout.size() >= 3) strip.holes.push_back(std::move(cutout));
        }
        result.push_back(std::move(strip));
    }
    return result;
}

std::vector<Interval> ringIntervals(const Ring &ring, double x)
{
    std::vector<double> crossings;
    for (std::size_t i = 0; i < ring.size(); ++i)
    {
        const Point &a = ring[i];
        const Point &b = ring[(i + 1) % ring.size()];
        if ((a.x <= x && b.x > x) || (b.x <= x && a.x > x))
            crossings.push_back(a.y + (x - a.x) * (b.y - a.y) / (b.x - a.x));
    }
    std::sort(crossings.begin(), crossings.end());
    std::vector<Interval> intervals;
    for (std::size_t i = 1; i < crossings.size(); i += 2)
        if (crossings[i] > crossings[i - 1])
            intervals.emplace_back(crossings[i - 1], crossings[i]);
    return intervals;
}

std::vector<Interval> unionIntervals(std::vector<Interval> intervals)
{
    std::sort(intervals.begin(), intervals.end());
    std::vector<Interval> result;
    for (const Interval &interval : intervals)
    {
        if (!result.empty() && interval.first <= result.back().second)
            result.back().second = std::max(result.back().second, interval.second);
        else result.push_back(interval);
    }
    return result;
}

std::vector<Interval> differenceIntervals(const std::vector<Interval> &source,
                                          const std::vector<Interval> &cutters)
{
    std::vector<Interval> result;
    for (const Interval &interval : source)
    {
        double start = interval.first;
        for (const Interval &cutter : cutters)
        {
            if (cutter.second <= start || cutter.first >= interval.second) continue;
            if (cutter.first > start)
                result.emplace_back(start, std::min(cutter.first, interval.second));
            start = std::max(start, cutter.second);
            if (start >= interval.second) break;
        }
        if (start < interval.second) result.emplace_back(start, interval.second);
    }
    return result;
}

struct ClippedPolygon
{
    Ring exterior;
    std::vector<Ring> holes;
};

void addEdges(const Ring &ring, std::vector<Edge> *edges,
              std::vector<double> *xs)
{
    for (std::size_t i = 0; i < ring.size(); ++i)
    {
        const Point &a = ring[i];
        const Point &b = ring[(i+1) % ring.size()];
        xs->push_back(a.x);
        if (std::abs(a.x - b.x) > 1.0e-14) edges->push_back({a,b});
    }
}

double coveredLengthAt(const std::vector<ClippedPolygon> &polygons, double x)
{
    std::vector<Interval> union_parts;
    for (const ClippedPolygon &polygon : polygons)
    {
        std::vector<Interval> parts = ringIntervals(polygon.exterior, x);
        std::vector<Interval> cutouts;
        for (const Ring &hole : polygon.holes)
        {
            std::vector<Interval> sections = ringIntervals(hole, x);
            cutouts.insert(cutouts.end(), sections.begin(), sections.end());
        }
        if (!cutouts.empty())
            parts = differenceIntervals(parts, unionIntervals(std::move(cutouts)));
        union_parts.insert(union_parts.end(), parts.begin(), parts.end());
    }
    double length = 0.0;
    for (const Interval &interval : unionIntervals(std::move(union_parts)))
        length += std::max(0.0, interval.second - interval.first);
    return length;
}

// Integrate the union of clipped polygon vertical spans. Critical x-events
// are vertex positions and all projected segment intersections. Between
// events, the boundaries are linear, so trapezoidal integration is exact.
double cellCoverage(const std::vector<RowPolygon> &polygons, double x)
{
    std::vector<ClippedPolygon> clipped;
    std::vector<double> breaks{x, x+1.0};
    std::vector<Edge> edges;
    for (const RowPolygon &polygon : polygons)
    {
        if (polygon.east <= x || polygon.west >= x + 1.0) continue;
        ClippedPolygon local;
        local.exterior = clipToColumn(polygon.exterior, x);
        if (local.exterior.size() < 3) continue;
        addEdges(local.exterior, &edges, &breaks);
        for (const Ring &hole : polygon.holes)
        {
            Ring cutout = clipToColumn(hole, x);
            if (cutout.size() >= 3)
            {
                addEdges(cutout, &edges, &breaks);
                local.holes.push_back(std::move(cutout));
            }
        }
        clipped.push_back(std::move(local));
    }
    if (clipped.empty()) return 0.0;
    if (edges.size() > 256)
        throw std::invalid_argument("Rainfall polygon too complex within a single DEM cell");
    for (std::size_t i = 0; i < edges.size(); ++i)
    {
        const Edge &a = edges[i];
        const double a_dx = a.b.x - a.a.x;
        const double a_dy = a.b.y - a.a.y;
        for (std::size_t j = i+1; j < edges.size(); ++j)
        {
            const Edge &b = edges[j];
            const double b_dx = b.b.x - b.a.x;
            const double b_dy = b.b.y - b.a.y;
            const double denominator = a_dx * b_dy - a_dy * b_dx;
            if (std::abs(denominator) < 1.0e-14) continue;
            const double rx = b.a.x - a.a.x;
            const double ry = b.a.y - a.a.y;
            const double t = (rx * b_dy - ry * b_dx) / denominator;
            const double u = (rx * a_dy - ry * a_dx) / denominator;
            if (t > 0.0 && t < 1.0 && u > 0.0 && u < 1.0)
                breaks.push_back(a.a.x + t * a_dx);
        }
    }
    std::sort(breaks.begin(), breaks.end());
    breaks.erase(std::unique(breaks.begin(), breaks.end(),
        [](double a, double b) { return std::abs(a-b) < 1.0e-12; }), breaks.end());
    double area = 0.0;
    for (std::size_t i = 1; i < breaks.size(); ++i)
    {
        const double left = std::max(x, breaks[i-1]);
        const double right = std::min(x+1.0, breaks[i]);
        const double width = right - left;
        if (width <= 1.0e-12) continue;
        const double a = coveredLengthAt(clipped, left + width * 1.0e-10);
        const double b = coveredLengthAt(clipped, right - width * 1.0e-10);
        area += width * (a+b) * 0.5;
    }
    if (!std::isfinite(area) || area < -1.0e-8 || area > 1.0 + 1.0e-8)
        throw std::runtime_error("Invalid polygon union area at rainfall cell");
    return std::clamp(area, 0.0, 1.0);
}
} // namespace

RainfallRunoff calculateRainfallRunoff(const ElevationGrid &grid,
    const RainfallScenario &scenario, std::uint64_t maximum_examined_cells,
    const std::atomic<bool> *cancel_requested)
{
    validateRasterGeometry(grid.geometry, grid.columns, grid.rows);
    const std::uint64_t total = std::uint64_t(grid.columns) * grid.rows;
    if (grid.elevations_m.size() != total || scenario.polygons.empty() ||
        scenario.polygons.size() > 32 || !std::isfinite(scenario.depth_mm) ||
        scenario.depth_mm < 0.0 || scenario.depth_mm > 100000.0 ||
        !std::isfinite(scenario.duration_seconds) ||
        scenario.duration_seconds <= 0.0 || scenario.duration_seconds > 31557600.0 ||
        !std::isfinite(scenario.effective_runoff_fraction) ||
        scenario.effective_runoff_fraction < 0.0 ||
        scenario.effective_runoff_fraction > 1.0 ||
        maximum_examined_cells == 0 || maximum_examined_cells > 10000000)
        throw std::invalid_argument("Invalid rainfall scenario or DEM grid");

    std::vector<Polygon> polygons;
    polygons.reserve(scenario.polygons.size());
    std::uint64_t vertices = 0;
    double west = double(grid.columns);
    double east = 0.0;
    double north = double(grid.rows);
    double south = 0.0;
    for (const GeographicPolygon &source : scenario.polygons)
    {
        if (source.holes.size() > 64)
            throw std::invalid_argument("Too many rainfall polygon holes");
        Polygon polygon;
        polygon.exterior = projectRing(grid.geometry, source.exterior, &vertices);
        polygon.west = polygon.east = polygon.exterior.front().x;
        polygon.north = polygon.south = polygon.exterior.front().y;
        for (const Point &point : polygon.exterior)
        {
            polygon.west = std::min(polygon.west, point.x);
            polygon.east = std::max(polygon.east, point.x);
            polygon.north = std::min(polygon.north, point.y);
            polygon.south = std::max(polygon.south, point.y);
        }
        for (const GeographicRing &ring : source.holes)
            polygon.holes.push_back(projectRing(grid.geometry, ring, &vertices));
        west = std::min(west, polygon.west);
        east = std::max(east, polygon.east);
        north = std::min(north, polygon.north);
        south = std::max(south, polygon.south);
        polygons.push_back(std::move(polygon));
    }

    RainfallRunoff result;
    result.covered_fraction.resize(std::size_t(total), 0.0f);
    result.runoff_volume_m3.resize(std::size_t(total), 0.0);
    result.touches_domain_boundary = west < 0.0 || north < 0.0 ||
                                      east > grid.columns || south > grid.rows;
    if (east <= 0 || west >= grid.columns || south <= 0 || north >= grid.rows)
        return result;
    const std::uint32_t x0 = std::uint32_t(std::max(0.0, std::floor(west)));
    const std::uint32_t x1 = std::uint32_t(std::min(double(grid.columns), std::ceil(east)));
    const std::uint32_t y0 = std::uint32_t(std::max(0.0, std::floor(north)));
    const std::uint32_t y1 = std::uint32_t(std::min(double(grid.rows), std::ceil(south)));
    if (std::uint64_t(x1-x0) * (y1-y0) > maximum_examined_cells)
        throw std::invalid_argument("Too many DEM cells within rainfall polygon bounding box");

    long double area_sum = 0.0;
    const double depth_m = scenario.depth_mm / 1000.0;
    for (std::uint32_t y = y0; y < y1; ++y)
    {
        if (cancel_requested != nullptr && cancel_requested->load())
            throw std::runtime_error("Rainfall calculation canceled");
        const std::vector<RowPolygon> row = rowPolygons(polygons, double(y));
        if (row.empty()) continue;
        const double cell_area = rasterRowCellAreaM2(grid.geometry, grid.columns, y);
        for (std::uint32_t x = x0; x < x1; ++x)
        {
            const double fraction = cellCoverage(row, double(x));
            if (fraction <= 1.0e-9) continue;
            const std::size_t index = std::size_t(y) * grid.columns + x;
            if (!std::isfinite(grid.elevations_m[index]))
            {
                result.contains_missing_dem_coverage = true;
                continue;
            }
            result.covered_fraction[index] = float(fraction);
            result.runoff_volume_m3[index] = cell_area * fraction * depth_m *
                                              scenario.effective_runoff_fraction;
            area_sum += cell_area * fraction;
            ++result.wet_cells;
            if (x == 0 || y == 0 || x + 1 == grid.columns || y + 1 == grid.rows)
                result.touches_domain_boundary = true;
        }
    }
    result.covered_area_m2 = double(area_sum);
    result.gross_rainfall_volume_m3 = double(area_sum * depth_m);
    result.effective_runoff_volume_m3 = double(area_sum * depth_m *
                                                scenario.effective_runoff_fraction);
    result.retained_or_lost_volume_m3 = result.gross_rainfall_volume_m3 -
                                        result.effective_runoff_volume_m3;
    result.average_effective_runoff_m3_s = result.effective_runoff_volume_m3 /
                                            scenario.duration_seconds;
    return result;
}
} // namespace Aowis::Hydrology
