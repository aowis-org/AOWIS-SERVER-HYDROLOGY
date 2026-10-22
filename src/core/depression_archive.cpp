#include <aowis/hydrology/depression_archive.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Aowis::Hydrology
{
namespace
{
constexpr std::uint8_t Magic[8] = {'A','O','W','H','D','H','0','1'};
constexpr std::uint64_t HeaderBytes = 33;
constexpr std::uint64_t BasinBytes = 81;
constexpr std::uint64_t TrailerBytes = 8;
constexpr std::uint64_t FnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t FnvPrime = 1099511628211ULL;

std::uint64_t hashBytes(const std::vector<std::uint8_t> &bytes, std::size_t end)
{
    std::uint64_t hash = FnvOffset;
    for (std::size_t index = 0; index < end; ++index)
    {
        hash ^= bytes[index];
        hash *= FnvPrime;
    }
    return hash;
}

void appendUnsigned(std::vector<std::uint8_t> &out, std::uint64_t value, unsigned bytes)
{
    for (unsigned index = 0; index < bytes; ++index)
        out.push_back(std::uint8_t(value >> (index * 8)));
}

void appendDouble(std::vector<std::uint8_t> &out, double value)
{
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    appendUnsigned(out, bits, 8);
}

std::uint64_t readUnsigned(const std::vector<std::uint8_t> &source,
                           std::size_t *cursor, unsigned bytes)
{
    if (*cursor > source.size() || source.size() - *cursor < bytes)
        throw std::invalid_argument("Truncated depression hierarchy archive");
    std::uint64_t value = 0;
    for (unsigned index = 0; index < bytes; ++index)
        value |= std::uint64_t(source[(*cursor)++]) << (index * 8);
    return value;
}

double readDouble(const std::vector<std::uint8_t> &source, std::size_t *cursor)
{
    const std::uint64_t bits = readUnsigned(source, cursor, 8);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void validate(const DepressionHierarchy &hierarchy)
{
    const std::uint64_t cells = std::uint64_t(hierarchy.columns) * hierarchy.rows;
    if (hierarchy.columns < 2 || hierarchy.rows < 2 ||
        hierarchy.valid_cells > cells || hierarchy.basins.size() > 2 * cells)
        throw std::invalid_argument("Invalid depression hierarchy geometry");
    for (std::size_t index = 0; index < hierarchy.basins.size(); ++index)
    {
        const DepressionBasin &basin = hierarchy.basins[index];
        if (basin.index != index || basin.minimum_cell >= cells ||
            basin.spill_cell >= cells ||
            (basin.parent_index != -1 &&
             (basin.parent_index <= std::int64_t(index) ||
              std::uint64_t(basin.parent_index) >= hierarchy.basins.size())) ||
            !std::isfinite(basin.minimum_elevation_m) ||
            !std::isfinite(basin.formation_elevation_m) ||
            basin.minimum_elevation_m > basin.formation_elevation_m ||
            (std::isfinite(basin.spill_elevation_m) &&
             (basin.spill_elevation_m < basin.formation_elevation_m ||
              !std::isfinite(basin.capacity_at_spill_m3) ||
              basin.capacity_at_spill_m3 < 0.0 ||
              !std::isfinite(basin.area_below_spill_m2) ||
              basin.area_below_spill_m2 < 0.0 || basin.cells_below_spill > cells)) ||
            (!std::isfinite(basin.spill_elevation_m) &&
             (!std::isnan(basin.spill_elevation_m) ||
              !std::isnan(basin.capacity_at_spill_m3) ||
              !std::isnan(basin.area_below_spill_m2))))
            throw std::invalid_argument("Invalid depression hierarchy basin record");
    }
}
}

std::vector<std::uint8_t> encodeDepressionHierarchy(const DepressionHierarchy &hierarchy)
{
    validate(hierarchy);
    if (hierarchy.basins.size() > (std::numeric_limits<std::size_t>::max() - HeaderBytes - TrailerBytes) / BasinBytes)
        throw std::length_error("Depression hierarchy is too large to encode");
    std::vector<std::uint8_t> bytes;
    bytes.reserve(std::size_t(HeaderBytes + BasinBytes * hierarchy.basins.size() + TrailerBytes));
    bytes.insert(bytes.end(), Magic, Magic + 8);
    appendUnsigned(bytes, hierarchy.columns, 4);
    appendUnsigned(bytes, hierarchy.rows, 4);
    appendUnsigned(bytes, hierarchy.valid_cells, 8);
    appendUnsigned(bytes, hierarchy.has_open_boundary ? 1 : 0, 1);
    appendUnsigned(bytes, hierarchy.basins.size(), 8);
    for (const DepressionBasin &basin : hierarchy.basins)
    {
        appendUnsigned(bytes, basin.index, 8);
        appendUnsigned(bytes, std::uint64_t(basin.parent_index), 8);
        appendUnsigned(bytes, basin.minimum_cell, 8);
        appendDouble(bytes, basin.minimum_elevation_m);
        appendDouble(bytes, basin.formation_elevation_m);
        appendUnsigned(bytes, basin.spill_cell, 8);
        appendDouble(bytes, basin.spill_elevation_m);
        appendUnsigned(bytes, basin.spills_to_domain_boundary ? 1 : 0, 1);
        appendDouble(bytes, basin.capacity_at_spill_m3);
        appendDouble(bytes, basin.area_below_spill_m2);
        appendUnsigned(bytes, basin.cells_below_spill, 8);
    }
    appendUnsigned(bytes, hashBytes(bytes, bytes.size()), 8);
    return bytes;
}

DepressionHierarchy decodeDepressionHierarchy(const std::vector<std::uint8_t> &bytes,
                                              std::uint32_t expected_columns,
                                              std::uint32_t expected_rows)
{
    if (bytes.size() < HeaderBytes + TrailerBytes ||
        std::memcmp(bytes.data(), Magic, 8) != 0)
        throw std::invalid_argument("Unrecognized depression hierarchy archive");
    std::size_t cursor = 8;
    DepressionHierarchy hierarchy;
    hierarchy.columns = std::uint32_t(readUnsigned(bytes, &cursor, 4));
    hierarchy.rows = std::uint32_t(readUnsigned(bytes, &cursor, 4));
    hierarchy.valid_cells = readUnsigned(bytes, &cursor, 8);
    const std::uint64_t open = readUnsigned(bytes, &cursor, 1);
    const std::uint64_t count = readUnsigned(bytes, &cursor, 8);
    const std::uint64_t cells = std::uint64_t(expected_columns) * expected_rows;
    if (hierarchy.columns != expected_columns || hierarchy.rows != expected_rows ||
        expected_columns < 2 || expected_rows < 2 || open > 1 || count > 2 * cells ||
        count > (std::numeric_limits<std::size_t>::max() - HeaderBytes - TrailerBytes) / BasinBytes ||
        bytes.size() != HeaderBytes + BasinBytes * count + TrailerBytes)
        throw std::invalid_argument("Invalid depression hierarchy archive dimensions or size");
    hierarchy.has_open_boundary = open != 0;
    const std::size_t payload_end = bytes.size() - 8;
    const std::uint64_t checksum = hashBytes(bytes, payload_end);
    std::size_t trailer = payload_end;
    if (readUnsigned(bytes, &trailer, 8) != checksum)
        throw std::invalid_argument("Depression hierarchy archive checksum mismatch");
    hierarchy.basins.reserve(std::size_t(count));
    for (std::uint64_t index = 0; index < count; ++index)
    {
        DepressionBasin basin;
        basin.index = readUnsigned(bytes, &cursor, 8);
        const std::uint64_t parent_bits = readUnsigned(bytes, &cursor, 8);
        std::memcpy(&basin.parent_index, &parent_bits, sizeof(parent_bits));
        basin.minimum_cell = readUnsigned(bytes, &cursor, 8);
        basin.minimum_elevation_m = readDouble(bytes, &cursor);
        basin.formation_elevation_m = readDouble(bytes, &cursor);
        basin.spill_cell = readUnsigned(bytes, &cursor, 8);
        basin.spill_elevation_m = readDouble(bytes, &cursor);
        const std::uint64_t boundary = readUnsigned(bytes, &cursor, 1);
        if (boundary > 1)
            throw std::invalid_argument("Invalid basin boundary flag");
        basin.spills_to_domain_boundary = boundary != 0;
        basin.capacity_at_spill_m3 = readDouble(bytes, &cursor);
        basin.area_below_spill_m2 = readDouble(bytes, &cursor);
        basin.cells_below_spill = readUnsigned(bytes, &cursor, 8);
        hierarchy.basins.push_back(std::move(basin));
    }
    if (cursor != payload_end)
        throw std::invalid_argument("Depression hierarchy archive trailing data");
    validate(hierarchy);
    for (DepressionBasin &basin : hierarchy.basins)
    {
        if (basin.parent_index >= 0)
            hierarchy.basins[std::size_t(basin.parent_index)].child_indices.push_back(basin.index);
    }
    return hierarchy;
}
} // namespace Aowis::Hydrology
