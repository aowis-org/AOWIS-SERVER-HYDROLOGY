#include <aowis/hydrology/analysis.h>
#include <aowis/hydrology/depression_hierarchy.h>
#include <aowis/hydrology/redistribution.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

struct Arguments
{
    std::uint32_t columns = 256;
    std::uint32_t rows = 256;
    std::uint32_t seed = 1;
    std::string terrain = "rolling";
    double rain_mm = 35.0;
    double runoff_fraction = 0.65;
};

struct Stage
{
    std::string name;
    double wall_ms = 0.0;
    std::uint64_t resident_bytes = 0;
    bool resident_known = false;
};

// Linux-specific *process* memory, not the memory owned by one algorithm.
// Other platforms deliberately report null instead of a fabricated estimate.
bool residentBytes(const std::string &field, std::uint64_t *bytes)
{
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string token;
    while (status >> token)
    {
        if (token == field)
        {
            std::uint64_t kib = 0;
            std::string unit;
            if (!(status >> kib >> unit) || unit != "kB") return false;
            *bytes = kib * 1024;
            return true;
        }
        std::string rest;
        std::getline(status, rest);
    }
#else
    (void)field;
    (void)bytes;
#endif
    return false;
}

std::uint32_t parseUnsigned(const std::string &arg, const std::string &name,
                            std::uint32_t minimum, std::uint32_t maximum)
{
    if (arg.empty() || arg.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument(name + " must be an unsigned integer");
    std::size_t processed = 0;
    const unsigned long long parsed = std::stoull(arg, &processed, 10);
    if (processed != arg.size() || parsed < minimum || parsed > maximum)
        throw std::invalid_argument(name + " is outside its supported range");
    return std::uint32_t(parsed);
}

double parseReal(const std::string &arg, const std::string &name,
                 double minimum, double maximum)
{
    std::size_t processed = 0;
    const double parsed = std::stod(arg, &processed);
    if (processed != arg.size() || !std::isfinite(parsed) ||
        parsed < minimum || parsed > maximum)
        throw std::invalid_argument(name + " is outside its supported range");
    return parsed;
}

void printUsage()
{
    std::cout <<
        "Usage: aowis-hydrology-benchmark [options]\n"
        "  --columns N           DEM columns, 16..5000 (default 256)\n"
        "  --rows N              DEM rows, 16..5000 (default 256)\n"
        "  --seed N              Synthetic terrain seed, 0..4294967295 (default 1)\n"
        "  --terrain rolling|pitted|flat   Synthetic profile (default rolling)\n"
        "  --rain-mm MM          Uniform rainfall depth, 0..1000 (default 35)\n"
        "  --runoff-fraction F   Effective runoff fraction, 0..1 (default 0.65)\n"
        "  --help                Show this message\n"
        "At most 10,000,000 cells. Output is one JSON object on stdout.\n";
}

Arguments parseArguments(int argc, char **argv)
{
    Arguments args;
    for (int index = 1; index < argc; ++index)
    {
        const std::string name(argv[index]);
        if (name == "--help")
        {
            printUsage();
            std::exit(0);
        }
        if (index + 1 >= argc)
            throw std::invalid_argument("Missing value for " + name);
        const std::string value(argv[++index]);
        if (name == "--columns")
            args.columns = parseUnsigned(value, name, 16, 5000);
        else if (name == "--rows")
            args.rows = parseUnsigned(value, name, 16, 5000);
        else if (name == "--seed")
            args.seed = parseUnsigned(value, name, 0, std::numeric_limits<std::uint32_t>::max());
        else if (name == "--terrain")
        {
            if (value != "rolling" && value != "pitted" && value != "flat")
                throw std::invalid_argument("Unsupported terrain: " + value);
            args.terrain = value;
        }
        else if (name == "--rain-mm")
            args.rain_mm = parseReal(value, name, 0.0, 1000.0);
        else if (name == "--runoff-fraction")
            args.runoff_fraction = parseReal(value, name, 0.0, 1.0);
        else
            throw std::invalid_argument("Unknown option: " + name);
    }
    if (std::uint64_t(args.columns) * args.rows > 10000000)
        throw std::invalid_argument("Raster exceeds 10,000,000-cell benchmark budget");
    return args;
}

std::uint64_t noise(std::uint64_t number)
{
    // Integer-only counter hash. Neither STL distributions nor a random seed
    // from the machine affect the terrain's identity.
    number += 0x9e3779b97f4a7c15ULL;
    number = (number ^ (number >> 30)) * 0xbf58476d1ce4e5b9ULL;
    number = (number ^ (number >> 27)) * 0x94d049bb133111ebULL;
    return number ^ (number >> 31);
}

Aowis::Hydrology::ElevationGrid createGrid(const Arguments &args)
{
    Aowis::Hydrology::ElevationGrid result;
    result.columns = args.columns;
    result.rows = args.rows;
    result.geometry.horizontal_crs = Aowis::Hydrology::HorizontalCrs::Wgs84Utm;
    result.geometry.utm_zone = 32;
    result.geometry.northern_hemisphere = true;
    result.geometry.origin_x = 450000.0;
    result.geometry.origin_y = 5650000.0;
    result.geometry.pixel_width = 30.0;
    result.geometry.pixel_height = 30.0;
    result.geometry.vertical_datum = Aowis::Hydrology::VerticalDatum::Unknown;
    result.elevations_m.resize(std::size_t(args.columns) * args.rows);
    for (std::uint32_t y = 0; y < args.rows; ++y)
    {
        for (std::uint32_t x = 0; x < args.columns; ++x)
        {
            const std::uint64_t random = noise((std::uint64_t(y) << 32) ^ x ^
                                               (std::uint64_t(args.seed) << 1));
            double elevation = 250.0;
            if (args.terrain != "flat")
            {
                // Moderately sloped, corrugated synthetic surface with local
                // minima. Deliberately NOT a statistically validated DEM.
                elevation -= double(x) * 0.02 + double(y) * 0.025;
                elevation += 2.8 * std::sin(double(x) / 41.0) +
                             2.4 * std::cos(double(y) / 63.0);
                elevation += 0.0125 * double((random >> 24) & 255U);
                if (args.terrain == "pitted" &&
                    ((x % 96) > 37 && (x % 96) < 55) &&
                    ((y % 96) > 37 && (y % 96) < 55))
                    elevation -= 12.0;
            }
            result.elevations_m[std::size_t(y) * args.columns + x] = float(elevation);
        }
    }
    return result;
}

std::uint64_t elevationFingerprint(const std::vector<float> &elevations)
{
    std::uint64_t fingerprint = 14695981039346656037ULL;
    for (float elevation : elevations)
    {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(elevation);
        for (unsigned shift = 0; shift < 32; shift += 8)
        {
            fingerprint ^= (bits >> shift) & 255U;
            fingerprint *= 1099511628211ULL;
        }
    }
    return fingerprint;
}

template<typename Callback>
Stage measure(const std::string &name, Callback callback)
{
    const Clock::time_point started = Clock::now();
    callback();
    const Clock::time_point finished = Clock::now();
    Stage result;
    result.name = name;
    result.wall_ms = std::chrono::duration<double, std::milli>(finished - started).count();
    result.resident_known = residentBytes("VmRSS:", &result.resident_bytes);
    return result;
}

void memoryValue(std::uint64_t value, bool known)
{
    if (known) std::cout << value;
    else std::cout << "null";
}

void run(const Arguments &args)
{
    using namespace Aowis::Hydrology;
    std::vector<Stage> stages;
    ElevationGrid grid;
    stages.push_back(measure("synthetic_terrain", [&]() { grid = createGrid(args); }));
    const std::uint64_t fingerprint = elevationFingerprint(grid.elevations_m);

    AnalysisResult conditioned;
    stages.push_back(measure("d8_conditioning", [&]() { conditioned = analyze(grid); }));
    // Explicitly release this separate large result before hierarchy/routing.
    // This measures the usual sequential workload, not simultaneous retention
    // of every analysis artifact in a service process.
    const std::uint64_t conditioned_cells = conditioned.conditioned_cell_count;
    conditioned = AnalysisResult{};

    DepressionHierarchy hierarchy;
    stages.push_back(measure("depression_hierarchy", [&]() {
        hierarchy = buildDepressionHierarchy(grid);
    }));

    std::vector<double> runoff;
    stages.push_back(measure("uniform_runoff_raster", [&]() {
        runoff.resize(grid.elevations_m.size());
        for (std::uint32_t y = 0; y < grid.rows; ++y)
        {
            const double area_m2 = rasterRowCellAreaM2(grid.geometry, grid.columns, y);
            const double volume = area_m2 * args.rain_mm * 0.001 * args.runoff_fraction;
            std::fill(runoff.begin() + std::size_t(y) * grid.columns,
                      runoff.begin() + std::size_t(y + 1) * grid.columns, volume);
        }
    }));

    RunoffRedistribution routed;
    stages.push_back(measure("static_redistribution", [&]() {
        routed = redistributeRunoff(grid, hierarchy, runoff);
    }));

    const double tolerance_m3 = 1e-7 * std::max(1.0, routed.generated_runoff_m3);
    if (!std::isfinite(routed.mass_balance_error_m3) ||
        std::abs(routed.mass_balance_error_m3) > tolerance_m3 ||
        std::abs(routed.generated_runoff_m3 - routed.basin_storage_m3 -
                 routed.total_boundary_outflow_m3) > tolerance_m3)
        throw std::runtime_error("Hydrology benchmark failed mass-conservation validation");

    std::uint64_t peak = 0;
    const bool peak_known = residentBytes("VmHWM:", &peak);
    double measured_total_ms = 0.0;
    for (const Stage &stage : stages) measured_total_ms += stage.wall_ms;
    std::cout << std::setprecision(10);
    std::cout << "{\n  \"schema_version\": 1,\n  \"benchmark\": \"synthetic_hydrology_core\",\n"
              << "  \"columns\": " << args.columns << ",\n"
              << "  \"rows\": " << args.rows << ",\n"
              << "  \"cells\": " << std::uint64_t(args.columns) * args.rows << ",\n"
              << "  \"cell_size_m\": 30,\n  \"crs\": \"EPSG:32632\",\n"
              << "  \"terrain\": \"" << args.terrain << "\",\n"
              << "  \"seed\": " << args.seed << ",\n"
              << "  \"elevation_fingerprint_fnv64\": \"" << std::hex <<
                    std::setfill('0') << std::setw(16) << fingerprint << std::dec << "\",\n"
              << "  \"rain_mm\": " << args.rain_mm << ",\n"
              << "  \"runoff_fraction\": " << args.runoff_fraction << ",\n"
              << "  \"conditioned_cells\": " << conditioned_cells << ",\n"
              << "  \"basin_count\": " << hierarchy.basins.size() << ",\n"
              << "  \"generated_runoff_m3\": " << routed.generated_runoff_m3 << ",\n"
              << "  \"retained_m3\": " << routed.basin_storage_m3 << ",\n"
              << "  \"direct_boundary_outflow_m3\": " << routed.direct_boundary_outflow_m3 << ",\n"
              << "  \"basin_boundary_outflow_m3\": " << routed.basin_boundary_outflow_m3 << ",\n"
              << "  \"total_boundary_outflow_m3\": " << routed.total_boundary_outflow_m3 << ",\n"
              << "  \"mass_balance_error_m3\": " << routed.mass_balance_error_m3 << ",\n"
              << "  \"measured_stages_total_ms\": " << measured_total_ms << ",\n"
              << "  \"peak_process_rss_bytes\": ";
    memoryValue(peak, peak_known);
    std::cout << ",\n  \"stages\": [\n";
    for (std::size_t index = 0; index < stages.size(); ++index)
    {
        const Stage &stage = stages[index];
        std::cout << "    {\"name\": \"" << stage.name << "\", \"wall_ms\": "
                  << stage.wall_ms << ", \"process_rss_bytes\": ";
        memoryValue(stage.resident_bytes, stage.resident_known);
        std::cout << "}" << (index + 1 < stages.size() ? "," : "") << "\n";
    }
    std::cout << "  ]\n}\n";
}
}

int main(int argc, char **argv)
{
    try { run(parseArguments(argc, argv)); }
    catch (const std::exception &error)
    {
        std::cerr << "Hydrology benchmark: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
