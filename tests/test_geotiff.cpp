#include <aowis/hydrology/geotiff_source.h>

#include <gdal_priv.h>
#include <ogr_spatialref.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Aowis::Hydrology;
namespace fs = std::filesystem;

namespace
{
void writeFixture(const fs::path &path, double west, double north,
                  double resolution, std::uint32_t global_column_offset,
                  int missing_cell = -1)
{
    GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    assert(driver != nullptr);
    GDALDataset *dataset = driver->Create(path.string().c_str(), 4, 4, 1, GDT_Float32, nullptr);
    assert(dataset != nullptr);
    double transform[6] = {west, resolution, 0.0, north, 0.0, -resolution};
    assert(dataset->SetGeoTransform(transform) == CE_None);
    OGRSpatialReference srs;
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    assert(srs.importFromEPSG(4326) == OGRERR_NONE);
    assert(dataset->SetSpatialRef(&srs) == CE_None);
    GDALRasterBand *band = dataset->GetRasterBand(1);
    assert(band->SetNoDataValue(-9999.0) == CE_None);
    std::vector<float> samples(16);
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            samples[std::size_t(row * 4 + column)] = float(100 + row * 8 +
                                                          global_column_offset + column);
    if (missing_cell >= 0)
        samples[std::size_t(missing_cell)] = -9999.0f;
    assert(band->RasterIO(GF_Write, 0, 0, 4, 4, samples.data(),
                          4, 4, GDT_Float32, 0, 0, nullptr) == CE_None);
    GDALClose(dataset);
}

ElevationGrid requestedGrid()
{
    ElevationGrid result;
    result.columns = 8;
    result.rows = 4;
    result.geometry.horizontal_crs = HorizontalCrs::Wgs84Geographic;
    result.geometry.vertical_datum = VerticalDatum::Egm2008;
    result.geometry.origin_x = 8.0;
    result.geometry.origin_y = 51.0;
    result.geometry.pixel_width = 0.001;
    result.geometry.pixel_height = 0.001;
    return result;
}

bool failsWith(const fs::path &directory, const ElevationGrid &requested,
               const std::string &expected, const std::atomic<bool> *cancel = nullptr)
{
    try
    {
        (void)readGeoTiffTerrain(directory.string(), requested, cancel);
    }
    catch (const std::exception &error)
    {
        if (std::string(error.what()).find(expected) == std::string::npos)
            std::cerr << "Unexpected error: " << error.what() << '\n';
        return std::string(error.what()).find(expected) != std::string::npos;
    }
    return false;
}
}

int main()
{
    GDALAllRegister();
    const fs::path root = fs::temp_directory_path() /
        ("aowis-hydrology-dem-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{root};
    const fs::path tiles = root / "tiles";
    fs::create_directories(tiles);
    writeFixture(tiles / "west_DEM.tif", 8.0, 51.0, 0.001, 0);
    writeFixture(tiles / "east_DEM.tif", 8.004, 51.0, 0.001, 4);
    // A Copernicus ancillary TIFF is not an elevation raster; ignore it.
    writeFixture(tiles / "west_EDM.tif", 8.0, 51.0, 0.001, 300);

    const ElevationGrid requested = requestedGrid();
    const ElevationGrid mosaic = readGeoTiffTerrain(tiles.string(), requested);
    assert(mosaic.columns == 8 && mosaic.rows == 4);
    assert(mosaic.geometry.origin_x == requested.geometry.origin_x);
    assert(mosaic.elevations_m.size() == 32);
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 8; ++column)
            assert(mosaic.elevations_m[std::size_t(row * 8 + column)] ==
                   float(100 + row * 8 + column));

    // Request a subpixel-shifted grid: nearest-neighbour preserves source
    // elevations, never invents interpolated heights.
    ElevationGrid shifted = requested;
    shifted.columns = 2;
    shifted.rows = 2;
    shifted.geometry.origin_x = 8.0012;
    shifted.geometry.origin_y = 50.9988;
    const ElevationGrid partial = readGeoTiffTerrain(tiles.string(), shifted);
    assert(partial.elevations_m[0] == 110.0f || partial.elevations_m[0] == 109.0f);

    fs::remove(tiles / "east_DEM.tif");
    assert(failsWith(tiles, requested, "coverage incomplete"));
    writeFixture(tiles / "east_DEM.tif", 8.004, 51.0, 0.001, 4, 0);
    assert(failsWith(tiles, requested, "coverage incomplete"));
    writeFixture(tiles / "east_DEM.tif", 8.004, 51.0, 0.001, 4);

    std::atomic<bool> cancellation{true};
    assert(failsWith(tiles, requested, "canceled", &cancellation));
    ElevationGrid invalid = requested;
    invalid.geometry.vertical_datum = VerticalDatum::Unknown;
    assert(failsWith(tiles, invalid, "EGM2008"));
    invalid = requested;
    invalid.geometry.horizontal_crs = HorizontalCrs::Wgs84Utm;
    invalid.geometry.utm_zone = 32;
    assert(failsWith(tiles, invalid, "WGS84 geographic output"));

    // Overlapping source tiles would otherwise select values by filename
    // order, making analyses sensitive to directory layout.
    writeFixture(tiles / "duplicate_DEM.tif", 8.0, 51.0, 0.001, 50);
    assert(failsWith(tiles, requested, "Overlapping source DEM"));
    fs::remove(tiles / "duplicate_DEM.tif");

    // A shifted source raster must not be silently merged into another grid.
    writeFixture(tiles / "east_DEM.tif", 8.0045, 51.0, 0.001, 4);
    assert(failsWith(tiles, requested, "pixel-aligned"));
    std::cout << "GeoTIFF source tests passed\n";
}
