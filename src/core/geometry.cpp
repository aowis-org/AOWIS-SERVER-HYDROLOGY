#include <aowis/hydrology/geometry.h>

#include <GeographicLib/Geodesic.hpp>
#include <GeographicLib/PolygonArea.hpp>
#include <GeographicLib/UTMUPS.hpp>

#include <cmath>
#include <stdexcept>

namespace Aowis::Hydrology
{
namespace
{
const GeographicLib::Geodesic &ellipsoid()
{
    return GeographicLib::Geodesic::WGS84();
}

double projectionScale(const RasterGeometry &geometry,
                       std::uint32_t columns, std::uint32_t row)
{
    const MapCoordinate centre = pixelCentre(geometry, columns / 2, row);
    double latitude = 0.0;
    double longitude = 0.0;
    double convergence = 0.0;
    double scale = 0.0;
    GeographicLib::UTMUPS::Reverse(geometry.utm_zone,
                                   geometry.northern_hemisphere,
                                   centre.x, centre.y,
                                   latitude, longitude, convergence, scale);
    if (!std::isfinite(scale) || scale <= 0.0)
        throw std::invalid_argument("UTM raster lies outside valid projection extent");
    return scale;
}
}

UtmCoordinate wgs84ToUtm(double longitude_deg, double latitude_deg)
{
    if (!std::isfinite(longitude_deg) || !std::isfinite(latitude_deg) ||
        longitude_deg < -180.0 || longitude_deg > 180.0 ||
        latitude_deg < -80.0 || latitude_deg >= 84.0)
        throw std::invalid_argument("WGS84 UTM requires longitude [-180,180] and latitude [-80,84)");
    UtmCoordinate result;
    GeographicLib::UTMUPS::Forward(latitude_deg, longitude_deg, result.zone,
                                   result.north, result.easting_m, result.northing_m);
    if (result.zone < 1 || result.zone > 60)
        throw std::invalid_argument("UTM zone not available at this coordinate");
    return result;
}

MapCoordinate utmToWgs84(const UtmCoordinate &utm)
{
    if (utm.zone < 1 || utm.zone > 60 || !std::isfinite(utm.easting_m) ||
        !std::isfinite(utm.northing_m))
        throw std::invalid_argument("Invalid WGS84 UTM coordinate");
    double latitude = 0.0;
    double longitude = 0.0;
    GeographicLib::UTMUPS::Reverse(utm.zone, utm.north, utm.easting_m,
                                   utm.northing_m, latitude, longitude);
    return {longitude, latitude};
}

void validateRasterGeometry(const RasterGeometry &geometry,
                            std::uint32_t columns, std::uint32_t rows)
{
    if (columns < 2 || rows < 2 || columns > 100000 || rows > 100000 ||
        !std::isfinite(geometry.origin_x) || !std::isfinite(geometry.origin_y) ||
        !std::isfinite(geometry.pixel_width) || !std::isfinite(geometry.pixel_height) ||
        geometry.pixel_width <= 0.0 || geometry.pixel_height <= 0.0 ||
        !std::isfinite(geometry.origin_x + columns * geometry.pixel_width) ||
        !std::isfinite(geometry.origin_y - rows * geometry.pixel_height))
        throw std::invalid_argument("Invalid raster geometry or extent");
    if (geometry.horizontal_crs == HorizontalCrs::Wgs84Geographic)
    {
        if (geometry.utm_zone != 0 || geometry.origin_x < -180.0 ||
            geometry.origin_x + columns * geometry.pixel_width > 180.0 ||
            geometry.origin_y > 85.0 ||
            geometry.origin_y - rows * geometry.pixel_height < -85.0)
            throw std::invalid_argument("Invalid north-up WGS84 geographic raster extent");
    }
    else if (geometry.horizontal_crs == HorizontalCrs::Wgs84Utm)
    {
        if (geometry.utm_zone < 1 || geometry.utm_zone > 60)
            throw std::invalid_argument("Invalid UTM zone");
        // Check that a representative coordinate is within the projection domain.
        const MapCoordinate representative = pixelCentre(geometry, columns / 2, rows / 2);
        const MapCoordinate check = utmToWgs84({geometry.utm_zone,
            geometry.northern_hemisphere, representative.x, representative.y});
        if (!std::isfinite(check.x) || !std::isfinite(check.y))
            throw std::invalid_argument("Invalid UTM raster coordinates");
    }
    else
        throw std::invalid_argument("Unsupported horizontal coordinate reference system");
}

MapCoordinate pixelCentre(const RasterGeometry &geometry,
                          std::uint32_t column, std::uint32_t row)
{
    return {geometry.origin_x + (double(column) + 0.5) * geometry.pixel_width,
            geometry.origin_y - (double(row) + 0.5) * geometry.pixel_height};
}

MapCoordinate pixelCentreWgs84(const RasterGeometry &geometry,
                               std::uint32_t column, std::uint32_t row)
{
    const MapCoordinate point = pixelCentre(geometry, column, row);
    if (geometry.horizontal_crs == HorizontalCrs::Wgs84Geographic)
        return point;
    return utmToWgs84({geometry.utm_zone, geometry.northern_hemisphere,
                       point.x, point.y});
}

double rasterRowCellAreaM2(const RasterGeometry &geometry,
                           std::uint32_t columns, std::uint32_t row)
{
    if (geometry.horizontal_crs == HorizontalCrs::Wgs84Utm)
    {
        const double scale = projectionScale(geometry, columns, row);
        return geometry.pixel_width * geometry.pixel_height / (scale * scale);
    }

    const double north = geometry.origin_y - double(row) * geometry.pixel_height;
    const double south = north - geometry.pixel_height;
    const double west = geometry.origin_x;
    const double east = west + geometry.pixel_width;
    GeographicLib::PolygonArea polygon(ellipsoid());
    polygon.AddPoint(north, west);
    polygon.AddPoint(north, east);
    polygon.AddPoint(south, east);
    polygon.AddPoint(south, west);
    double perimeter = 0.0;
    double signed_area = 0.0;
    polygon.Compute(false, true, perimeter, signed_area);
    return std::abs(signed_area);
}

double rasterRowNeighborDistanceM(const RasterGeometry &geometry,
                                   std::uint32_t columns, std::uint32_t row,
                                   int offset_x, int offset_y)
{
    if ((offset_x == 0 && offset_y == 0) || std::abs(offset_x) > 1 ||
        std::abs(offset_y) > 1)
        throw std::invalid_argument("Unsupported raster neighbor displacement");
    if (geometry.horizontal_crs == HorizontalCrs::Wgs84Utm)
    {
        const double scale = projectionScale(geometry, columns, row);
        return std::hypot(offset_x * geometry.pixel_width,
                          offset_y * geometry.pixel_height) / scale;
    }
    const MapCoordinate first = pixelCentre(geometry, columns / 2, row);
    const double latitude2 = first.y - offset_y * geometry.pixel_height;
    const double longitude2 = first.x + offset_x * geometry.pixel_width;
    double distance = 0.0;
    ellipsoid().Inverse(first.y, first.x, latitude2, longitude2, distance);
    if (!std::isfinite(distance) || distance <= 0.0)
        throw std::invalid_argument("Invalid geodesic cell distance");
    return distance;
}
} // namespace Aowis::Hydrology
