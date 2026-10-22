#ifndef AOWIS_HYDROLOGY_GEOMETRY_H
#define AOWIS_HYDROLOGY_GEOMETRY_H

#include <cstdint>

namespace Aowis::Hydrology
{

// Horizontal CRS describes cell-centre coordinates, not elevation heights.
// UTM supports WGS84 zones 1..60 in either hemisphere; elevation datum is
// separate and is never modified by horizontal coordinate conversions.
enum class HorizontalCrs : std::uint8_t
{
    Wgs84Geographic,
    Wgs84Utm
};

enum class VerticalDatum : std::uint8_t
{
    Unknown,
    Egm2008,
    EllipsoidalWgs84
};

struct RasterGeometry
{
    HorizontalCrs horizontal_crs = HorizontalCrs::Wgs84Geographic;
    VerticalDatum vertical_datum = VerticalDatum::Unknown;
    int utm_zone = 0;                     // Only used for Wgs84Utm.
    bool northern_hemisphere = true;     // Only used for Wgs84Utm.
    double origin_x = 0.0;               // Western OUTER cell edge (deg or metres).
    double origin_y = 0.0;               // Northern OUTER cell edge (deg or metres).
    double pixel_width = 0.0;            // Positive eastward step (deg or metres).
    double pixel_height = 0.0;           // Positive southward step (deg or metres).
};

struct MapCoordinate
{
    double x = 0.0;
    double y = 0.0;
};

struct UtmCoordinate
{
    int zone = 0;
    bool north = true;
    double easting_m = 0.0;
    double northing_m = 0.0;
};

// GeographicLib WGS84 conversions. Strictly horizontal; never apply these
// functions to EGM2008 terrain heights.
UtmCoordinate wgs84ToUtm(double longitude_deg, double latitude_deg);
MapCoordinate utmToWgs84(const UtmCoordinate &utm);

void validateRasterGeometry(const RasterGeometry &geometry,
                            std::uint32_t columns, std::uint32_t rows);
MapCoordinate pixelCentre(const RasterGeometry &geometry,
                          std::uint32_t column, std::uint32_t row);
MapCoordinate pixelCentreWgs84(const RasterGeometry &geometry,
                               std::uint32_t column, std::uint32_t row);

// Actual WGS84 ellipsoid metrics for north-up geographic grids; WGS84 UTM
// grid metrics are adjusted by GeographicLib's local projection scale.
// The metrics are evaluated at a representative column, so UTM areas are
// approximations over wide zones (not flood survey volumes).
double rasterRowCellAreaM2(const RasterGeometry &geometry,
                           std::uint32_t columns, std::uint32_t row);
double rasterRowNeighborDistanceM(const RasterGeometry &geometry,
                                   std::uint32_t columns, std::uint32_t row,
                                   int offset_x, int offset_y);

} // namespace Aowis::Hydrology
#endif
