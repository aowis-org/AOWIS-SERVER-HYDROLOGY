#include <aowis/hydrology/geotiff_source.h>

#include <gdal_priv.h>
#include <gdal_utils.h>
#include <ogr_spatialref.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace Aowis::Hydrology
{
namespace
{
namespace fs = std::filesystem;

struct DatasetCloser
{
    void operator()(GDALDataset *dataset) const
    {
        if (dataset != nullptr)
            GDALClose(dataset);
    }
};
using DatasetHandle = std::unique_ptr<GDALDataset, DatasetCloser>;

struct WarpOptionsCloser
{
    void operator()(GDALWarpAppOptions *options) const
    {
        if (options != nullptr)
            GDALWarpAppOptionsFree(options);
    }
};
using WarpOptionsHandle = std::unique_ptr<GDALWarpAppOptions, WarpOptionsCloser>;

void checkCancellation(const std::atomic<bool> *cancel_requested)
{
    if (cancel_requested != nullptr && cancel_requested->load())
        throw std::runtime_error("Terrain reading canceled");
}

int progressCallback(double, const char *, void *argument)
{
    const std::atomic<bool> *cancellation =
        static_cast<const std::atomic<bool> *>(argument);
    return cancellation == nullptr || !cancellation->load();
}

std::string decimal(double value)
{
    std::array<char, 64> buffer{};
    const std::to_chars_result result = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general, 17);
    if (result.ec != std::errc())
        throw std::runtime_error("Cannot serialize terrain grid coordinate");
    return std::string(buffer.data(), result.ptr);
}

bool approximatelyEqual(double a, double b)
{
    return std::abs(a - b) <= 1e-8 * std::max({1.0, std::abs(a), std::abs(b)});
}

struct SourceRaster
{
    DatasetHandle dataset;
    std::array<double, 6> transform{};
    std::string name;
    double west = 0.0;
    double east = 0.0;
    double south = 0.0;
    double north = 0.0;
};


} // namespace

ElevationGrid readGeoTiffTerrain(const std::string &source_directory,
                                 const ElevationGrid &requested,
                                 const std::atomic<bool> *cancel_requested)
{
    checkCancellation(cancel_requested);
    validateRasterGeometry(requested.geometry, requested.columns, requested.rows);
    if (requested.geometry.horizontal_crs != HorizontalCrs::Wgs84Geographic)
        throw std::invalid_argument("Local GLO-30 GeoTIFF source requires WGS84 geographic output");
    if (requested.columns < 1 || requested.rows < 1 ||
        std::uint64_t(requested.columns) * requested.rows > 100000000ULL)
        throw std::invalid_argument("Invalid requested DEM dimensions");
    if (requested.geometry.vertical_datum != VerticalDatum::Egm2008)
        throw std::invalid_argument("Copernicus GeoTIFF analysis requires EGM2008 heights");

    const fs::path root = fs::weakly_canonical(fs::path(source_directory));
    if (source_directory.empty() || !fs::is_directory(root))
        throw std::invalid_argument("Configured GeoTIFF source directory does not exist");

    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator iterator(root, fs::directory_options::skip_permission_denied),
                                          end; iterator != end; ++iterator)
    {
        checkCancellation(cancel_requested);
        if (iterator->is_symlink())
        {
            iterator.disable_recursion_pending();
            continue;
        }
        if (!iterator->is_regular_file())
            continue;
        std::string extension = iterator->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char character) { return char(std::tolower(character)); });
        if (extension != ".tif" && extension != ".tiff")
            continue;
        files.push_back(iterator->path());
        if (files.size() > 4096)
            throw std::invalid_argument("Too many GeoTIFF sources in configured directory");
    }
    // Copernicus delivery directories can contain additional TIFF layers
    // (edit mask, water mask, height error, etc.). If native *_DEM.tif files
    // are present, use only them; never reinterpret ancillary rasters as DEM.
    const auto isCopernicusDem = [](const fs::path &path)
    {
        std::string file_name = path.filename().string();
        std::transform(file_name.begin(), file_name.end(), file_name.begin(),
                       [](unsigned char character) { return char(std::tolower(character)); });
        return file_name.ends_with("_dem.tif") || file_name.ends_with("_dem.tiff");
    };
    if (std::any_of(files.begin(), files.end(), isCopernicusDem))
        std::erase_if(files, [&isCopernicusDem](const fs::path &file)
        {
            return !isCopernicusDem(file);
        });
    std::sort(files.begin(), files.end());
    if (files.empty())
        throw std::invalid_argument("No DEM GeoTIFFs found in configured source directory");

    GDALAllRegister();
    OGRSpatialReference wgs84;
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    if (wgs84.importFromEPSG(4326) != OGRERR_NONE)
        throw std::runtime_error("Unable to initialize EPSG:4326 spatial reference");

    std::vector<SourceRaster> rasters;
    rasters.reserve(files.size());
    std::array<double, 6> reference{};
    bool have_reference = false;
    for (const fs::path &file : files)
    {
        checkCancellation(cancel_requested);
        DatasetHandle dataset(static_cast<GDALDataset *>(GDALOpenEx(
            file.string().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr)));
        if (!dataset)
            throw std::runtime_error("Cannot read DEM GeoTIFF: " + file.string());
        if (dataset->GetRasterCount() != 1 || dataset->GetRasterXSize() < 1 ||
            dataset->GetRasterYSize() < 1)
            throw std::invalid_argument("Expected one-band elevation GeoTIFF: " + file.string());
        const OGRSpatialReference *spatial_ref = dataset->GetSpatialRef();
        if (spatial_ref == nullptr || !spatial_ref->IsSame(&wgs84))
            throw std::invalid_argument("GeoTIFF must use WGS84 EPSG:4326: " + file.string());
        SourceRaster raster;
        raster.name = file.string();
        if (dataset->GetGeoTransform(raster.transform.data()) != CE_None ||
            !std::isfinite(raster.transform[0]) || !std::isfinite(raster.transform[3]) ||
            !std::isfinite(raster.transform[1]) || !std::isfinite(raster.transform[5]) ||
            raster.transform[1] <= 0 || raster.transform[5] >= 0 ||
            raster.transform[2] != 0 || raster.transform[4] != 0)
            throw std::invalid_argument("DEM must be a north-up, nonrotated GeoTIFF: " + file.string());
        // Most regional source directories contain far more than the few
        // tiles needed for this job. Do not hold unrelated datasets open or
        // pass them to GDALWarp. Validate alignment for relevant tiles only.
        const double source_west = raster.transform[0];
        const double source_east = source_west +
            double(dataset->GetRasterXSize()) * raster.transform[1];
        const double source_north = raster.transform[3];
        const double source_south = source_north +
            double(dataset->GetRasterYSize()) * raster.transform[5];
        const double request_west = requested.geometry.origin_x;
        const double request_east = request_west +
            double(requested.columns) * requested.geometry.pixel_width;
        const double request_north = requested.geometry.origin_y;
        const double request_south = request_north -
            double(requested.rows) * requested.geometry.pixel_height;
        if (source_west >= request_east || source_east <= request_west ||
            source_south >= request_north || source_north <= request_south)
            continue;
        if (have_reference)
        {
            if (!approximatelyEqual(raster.transform[1], reference[1]) ||
                !approximatelyEqual(raster.transform[5], reference[5]))
                throw std::invalid_argument("Source GeoTIFF cell resolutions differ: " + file.string());
            const double offset_x = (raster.transform[0] - reference[0]) / reference[1];
            const double offset_y = (raster.transform[3] - reference[3]) / reference[5];
            if (std::abs(offset_x - std::round(offset_x)) > 0.001 ||
                std::abs(offset_y - std::round(offset_y)) > 0.001)
                throw std::invalid_argument("Source GeoTIFF grids are not pixel-aligned: " + file.string());
        }
        else
        {
            reference = raster.transform;
            have_reference = true;
        }
        raster.west = source_west;
        raster.east = source_east;
        raster.south = source_south;
        raster.north = source_north;
        // Two separate files covering the same cells make results depend on
        // filename ordering. Reject overlap instead of silently choosing one.
        for (const SourceRaster &existing : rasters)
        {
            const double intersection_width = std::min(raster.east, existing.east) -
                                              std::max(raster.west, existing.west);
            const double intersection_height = std::min(raster.north, existing.north) -
                                               std::max(raster.south, existing.south);
            if (intersection_width > raster.transform[1] * 0.001 &&
                intersection_height > -raster.transform[5] * 0.001)
                throw std::invalid_argument("Overlapping source DEM tiles: " +
                    existing.name + " and " + raster.name);
        }
        raster.dataset = std::move(dataset);
        rasters.push_back(std::move(raster));
    }

    if (rasters.empty())
        throw std::runtime_error("DEM coverage incomplete: no configured GeoTIFF overlaps the requested region");

    std::vector<GDALDatasetH> inputs;
    inputs.reserve(rasters.size());
    for (const SourceRaster &raster : rasters)
        inputs.push_back(raster.dataset.get());

    std::vector<std::string> args{
        "-of", "MEM", "-ot", "Float32", "-r", "near", "-t_srs", "EPSG:4326",
        "-te", decimal(requested.geometry.origin_x),
        decimal(requested.geometry.origin_y - requested.rows * requested.geometry.pixel_height),
        decimal(requested.geometry.origin_x + requested.columns * requested.geometry.pixel_width),
        decimal(requested.geometry.origin_y),
        "-ts", std::to_string(requested.columns), std::to_string(requested.rows),
        "-dstnodata", "nan", "-wo", "NUM_THREADS=1"
    };
    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (std::string &argument : args)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    WarpOptionsHandle options(GDALWarpAppOptionsNew(argv.data(), nullptr));
    if (!options)
        throw std::runtime_error("Failed to construct DEM mosaic sampling options");
    GDALWarpAppOptionsSetProgress(options.get(), progressCallback,
                                  const_cast<std::atomic<bool> *>(cancel_requested));
    int usage_error = 0;
    DatasetHandle mosaic(static_cast<GDALDataset *>(GDALWarp(
        "", nullptr, int(inputs.size()), inputs.data(), options.get(), &usage_error)));
    checkCancellation(cancel_requested);
    if (usage_error != 0 || !mosaic)
        throw std::runtime_error("GDAL could not mosaic/sample requested DEM grid");
    if (mosaic->GetRasterXSize() != int(requested.columns) ||
        mosaic->GetRasterYSize() != int(requested.rows))
        throw std::runtime_error("GDAL returned incorrect DEM grid dimensions");
    ElevationGrid output = requested;
    output.elevations_m.assign(std::size_t(requested.columns) * requested.rows,
                               std::numeric_limits<float>::quiet_NaN());
    GDALRasterBand *band = mosaic->GetRasterBand(1);
    // Read in rows to permit cancellation and avoid extra large copies.
    for (std::uint32_t row = 0; row < requested.rows; ++row)
    {
        checkCancellation(cancel_requested);
        const CPLErr read_status = band->RasterIO(GF_Read, 0, int(row), int(requested.columns), 1,
            output.elevations_m.data() + std::size_t(row) * requested.columns,
            int(requested.columns), 1, GDT_Float32, 0, 0, nullptr);
        if (read_status != CE_None)
            throw std::runtime_error("GDAL could not read sampled DEM data");
    }
    std::size_t missing = 0;
    std::size_t first_missing = 0;
    for (std::size_t index = 0; index < output.elevations_m.size(); ++index)
    {
        if (!std::isfinite(output.elevations_m[index]))
        {
            if (missing == 0)
                first_missing = index;
            ++missing;
        }
    }
    if (missing > 0)
    {
        const std::uint32_t column = std::uint32_t(first_missing % requested.columns);
        const std::uint32_t row = std::uint32_t(first_missing / requested.columns);
        throw std::runtime_error("DEM coverage incomplete: " + std::to_string(missing) +
            " cells missing; first missing at column " + std::to_string(column) +
            ", row " + std::to_string(row) +
            ". Expand the GeoTIFF source directory or reduce the analysis area.");
    }
    return output;
}

} // namespace Aowis::Hydrology
