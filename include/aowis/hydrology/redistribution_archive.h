#ifndef AOWIS_HYDROLOGY_REDISTRIBUTION_ARCHIVE_H
#define AOWIS_HYDROLOGY_REDISTRIBUTION_ARCHIVE_H

#include <aowis/hydrology/redistribution.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace Aowis::Hydrology
{

// Portable, little-endian fixed-record on-disk cache for the STATIC runoff
// redistribution model. Each record has its own integrity checksum so the
// server can validate a paginated range without reading every basin.
// Checksums detect accidental corruption; they are not authentication.
inline constexpr std::size_t RedistributionArchiveHeaderBytes = 140;
inline constexpr std::size_t RedistributionArchiveRecordBytes = 56;

struct RedistributionArchiveIdentity
{
    std::array<std::uint8_t, 16> job_uuid{};
    std::array<std::uint8_t, 16> scenario_uuid{};
    // Fingerprint of immutable job/scenario file metadata (size + mtime).
    std::array<std::uint8_t, 16> source_fingerprint{};
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
};

struct RedistributionArchiveHeader
{
    RedistributionArchiveIdentity identity;
    std::uint64_t basin_count = 0;
    RunoffRedistribution summary;
};

struct RedistributionArchiveRecord
{
    BasinRedistributionState state;
    std::int64_t parent_index = -1;
};

std::array<std::uint8_t, RedistributionArchiveHeaderBytes> encodeRedistributionHeader(
    const RedistributionArchiveIdentity &identity, const RunoffRedistribution &result);
RedistributionArchiveHeader decodeRedistributionHeader(
    const std::array<std::uint8_t, RedistributionArchiveHeaderBytes> &bytes);

std::array<std::uint8_t, RedistributionArchiveRecordBytes> encodeRedistributionRecord(
    const RedistributionArchiveRecord &record);
RedistributionArchiveRecord decodeRedistributionRecord(
    const std::array<std::uint8_t, RedistributionArchiveRecordBytes> &bytes,
    std::uint64_t expected_index, std::uint64_t basin_count);

} // namespace Aowis::Hydrology
#endif
