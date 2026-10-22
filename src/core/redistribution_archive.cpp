#include <aowis/hydrology/redistribution_archive.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace Aowis::Hydrology
{
namespace
{
constexpr std::uint8_t ArchiveMagic[8] = {'A', 'O', 'W', 'R', 'D', 'S', '0', '1'};
constexpr std::uint32_t ArchiveVersion = 1;

// FNV-1a is used only for inexpensive corruption detection.
std::uint64_t checksum(const std::uint8_t *bytes, std::size_t size)
{
    std::uint64_t value = 14695981039346656037ULL;
    for (std::size_t i = 0; i < size; ++i)
    {
        value ^= bytes[i];
        value *= 1099511628211ULL;
    }
    return value;
}

template <std::size_t N>
void write64(std::array<std::uint8_t, N> &bytes, std::size_t &position, std::uint64_t value)
{
    if (position + 8 > N) throw std::logic_error("Invalid redistribution archive encoder");
    for (unsigned int shift = 0; shift < 64; shift += 8)
        bytes[position++] = std::uint8_t(value >> shift);
}

template <std::size_t N>
void write32(std::array<std::uint8_t, N> &bytes, std::size_t &position, std::uint32_t value)
{
    if (position + 4 > N) throw std::logic_error("Invalid redistribution archive encoder");
    for (unsigned int shift = 0; shift < 32; shift += 8)
        bytes[position++] = std::uint8_t(value >> shift);
}

template <std::size_t N>
std::uint64_t read64(const std::array<std::uint8_t, N> &bytes, std::size_t &position)
{
    if (position + 8 > N) throw std::runtime_error("Corrupt redistribution cache field");
    std::uint64_t value = 0;
    for (unsigned int shift = 0; shift < 64; shift += 8)
        value |= std::uint64_t(bytes[position++]) << shift;
    return value;
}

template <std::size_t N>
std::uint32_t read32(const std::array<std::uint8_t, N> &bytes, std::size_t &position)
{
    if (position + 4 > N) throw std::runtime_error("Corrupt redistribution cache field");
    std::uint32_t value = 0;
    for (unsigned int shift = 0; shift < 32; shift += 8)
        value |= std::uint32_t(bytes[position++]) << shift;
    return value;
}

template <std::size_t N>
void writeDouble(std::array<std::uint8_t, N> &bytes, std::size_t &position, double value)
{
    if (!std::isfinite(value)) throw std::invalid_argument("Non-finite redistribution cache value");
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(value));
    write64(bytes, position, bits);
}

template <std::size_t N>
double readDouble(const std::array<std::uint8_t, N> &bytes, std::size_t &position)
{
    const std::uint64_t bits = read64(bytes, position);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(bits));
    if (!std::isfinite(value)) throw std::runtime_error("Non-finite redistribution cache value");
    return value;
}

template <std::size_t N>
void writeIdentityBytes(std::array<std::uint8_t, N> &bytes, std::size_t &position,
                        const std::array<std::uint8_t, 16> &uuid)
{
    if (position + uuid.size() > N) throw std::logic_error("Invalid redistribution cache layout");
    for (std::uint8_t value : uuid) bytes[position++] = value;
}

template <std::size_t N>
void readIdentityBytes(const std::array<std::uint8_t, N> &bytes, std::size_t &position,
                       std::array<std::uint8_t, 16> &uuid)
{
    if (position + uuid.size() > N) throw std::runtime_error("Invalid redistribution cache layout");
    for (std::uint8_t &value : uuid) value = bytes[position++];
}

void validateBalance(const RunoffRedistribution &result)
{
    const double scale = std::max(1.0, result.generated_runoff_m3);
    if (result.generated_runoff_m3 < 0.0 || result.basin_storage_m3 < 0.0 ||
        result.direct_boundary_outflow_m3 < 0.0 ||
        result.basin_boundary_outflow_m3 < 0.0 ||
        result.total_boundary_outflow_m3 < 0.0 ||
        std::abs(result.direct_boundary_outflow_m3 + result.basin_boundary_outflow_m3 -
                 result.total_boundary_outflow_m3) > 1.e-8 * scale ||
        std::abs(result.generated_runoff_m3 - result.basin_storage_m3 -
                 result.total_boundary_outflow_m3 - result.mass_balance_error_m3) > 1.e-8 * scale ||
        std::abs(result.mass_balance_error_m3) > 1.e-8 * scale)
        throw std::runtime_error("Redistribution cache violates volume balance");
}
}

std::array<std::uint8_t, RedistributionArchiveHeaderBytes> encodeRedistributionHeader(
    const RedistributionArchiveIdentity &identity, const RunoffRedistribution &result)
{
    if (identity.columns < 2 || identity.rows < 2)
        throw std::invalid_argument("Invalid redistribution cache grid");
    validateBalance(result);
    std::array<std::uint8_t, RedistributionArchiveHeaderBytes> bytes{};
    std::size_t position = 0;
    for (std::uint8_t value : ArchiveMagic) bytes[position++] = value;
    write32(bytes, position, ArchiveVersion);
    write32(bytes, position, identity.columns);
    write32(bytes, position, identity.rows);
    write64(bytes, position, std::uint64_t(result.basins.size()));
    writeIdentityBytes(bytes, position, identity.job_uuid);
    writeIdentityBytes(bytes, position, identity.scenario_uuid);
    writeIdentityBytes(bytes, position, identity.source_fingerprint);
    writeDouble(bytes, position, result.generated_runoff_m3);
    writeDouble(bytes, position, result.basin_storage_m3);
    writeDouble(bytes, position, result.direct_boundary_outflow_m3);
    writeDouble(bytes, position, result.basin_boundary_outflow_m3);
    writeDouble(bytes, position, result.total_boundary_outflow_m3);
    writeDouble(bytes, position, result.mass_balance_error_m3);
    write64(bytes, position, result.domain_boundary_is_artificial ? 1 : 0);
    if (position != RedistributionArchiveHeaderBytes - 8)
        throw std::logic_error("Incorrect redistribution cache header size");
    write64(bytes, position, checksum(bytes.data(), position));
    return bytes;
}

RedistributionArchiveHeader decodeRedistributionHeader(
    const std::array<std::uint8_t, RedistributionArchiveHeaderBytes> &bytes)
{
    if (!std::equal(std::begin(ArchiveMagic), std::end(ArchiveMagic), bytes.begin()))
        throw std::runtime_error("Invalid redistribution cache magic");
    std::size_t position = 8;
    if (read32(bytes, position) != ArchiveVersion)
        throw std::runtime_error("Unsupported redistribution cache version");
    RedistributionArchiveHeader header;
    header.identity.columns = read32(bytes, position);
    header.identity.rows = read32(bytes, position);
    header.basin_count = read64(bytes, position);
    readIdentityBytes(bytes, position, header.identity.job_uuid);
    readIdentityBytes(bytes, position, header.identity.scenario_uuid);
    readIdentityBytes(bytes, position, header.identity.source_fingerprint);
    header.summary.generated_runoff_m3 = readDouble(bytes, position);
    header.summary.basin_storage_m3 = readDouble(bytes, position);
    header.summary.direct_boundary_outflow_m3 = readDouble(bytes, position);
    header.summary.basin_boundary_outflow_m3 = readDouble(bytes, position);
    header.summary.total_boundary_outflow_m3 = readDouble(bytes, position);
    header.summary.mass_balance_error_m3 = readDouble(bytes, position);
    const std::uint64_t boundary = read64(bytes, position);
    if (boundary > 1) throw std::runtime_error("Invalid redistribution cache boundary flag");
    header.summary.domain_boundary_is_artificial = boundary == 1;
    const std::uint64_t stored = read64(bytes, position);
    if (position != bytes.size() || stored != checksum(bytes.data(), bytes.size() - 8) ||
        header.identity.columns < 2 || header.identity.rows < 2 ||
        header.basin_count > 2ULL * header.identity.columns * header.identity.rows)
        throw std::runtime_error("Invalid redistribution cache header checksum or dimensions");
    validateBalance(header.summary);
    return header;
}

std::array<std::uint8_t, RedistributionArchiveRecordBytes> encodeRedistributionRecord(
    const RedistributionArchiveRecord &record)
{
    if (record.parent_index < -1 ||
        (record.parent_index >= 0 &&
         std::uint64_t(record.parent_index) <= record.state.basin_index))
        throw std::invalid_argument("Invalid redistribution cache parent index");
    if (record.state.direct_inflow_m3 < 0.0 || record.state.stored_in_subtree_m3 < 0.0 ||
        record.state.overflow_m3 < 0.0)
        throw std::invalid_argument("Invalid redistribution cache volume");
    std::array<std::uint8_t, RedistributionArchiveRecordBytes> bytes{};
    std::size_t position = 0;
    write64(bytes, position, record.state.basin_index);
    write64(bytes, position, std::uint64_t(record.parent_index));
    writeDouble(bytes, position, record.state.direct_inflow_m3);
    writeDouble(bytes, position, record.state.stored_in_subtree_m3);
    writeDouble(bytes, position, record.state.overflow_m3);
    write64(bytes, position, record.state.at_spill ? 1 : 0);
    if (position != RedistributionArchiveRecordBytes - 8)
        throw std::logic_error("Incorrect redistribution cache record size");
    write64(bytes, position, checksum(bytes.data(), position));
    return bytes;
}

RedistributionArchiveRecord decodeRedistributionRecord(
    const std::array<std::uint8_t, RedistributionArchiveRecordBytes> &bytes,
    std::uint64_t expected_index, std::uint64_t basin_count)
{
    std::size_t position = 0;
    RedistributionArchiveRecord record;
    record.state.basin_index = read64(bytes, position);
    record.parent_index = std::bit_cast<std::int64_t>(read64(bytes, position));
    record.state.direct_inflow_m3 = readDouble(bytes, position);
    record.state.stored_in_subtree_m3 = readDouble(bytes, position);
    record.state.overflow_m3 = readDouble(bytes, position);
    const std::uint64_t flag = read64(bytes, position);
    const std::uint64_t stored = read64(bytes, position);
    if (position != bytes.size() || stored != checksum(bytes.data(), bytes.size() - 8) ||
        record.state.basin_index != expected_index ||
        record.parent_index < -1 ||
        (record.parent_index >= 0 &&
         (std::uint64_t(record.parent_index) <= expected_index ||
          std::uint64_t(record.parent_index) >= basin_count)) ||
        record.state.direct_inflow_m3 < 0.0 || record.state.stored_in_subtree_m3 < 0.0 ||
        record.state.overflow_m3 < 0.0 || flag > 1)
        throw std::runtime_error("Invalid redistribution cache record");
    record.state.at_spill = flag == 1;
    return record;
}

} // namespace Aowis::Hydrology
