#ifndef AOWIS_HYDROLOGY_DEPRESSION_ARCHIVE_H
#define AOWIS_HYDROLOGY_DEPRESSION_ARCHIVE_H

#include <aowis/hydrology/depression_hierarchy.h>

#include <cstdint>
#include <vector>

namespace Aowis::Hydrology
{

// Compact versioned little-endian serialization. The hierarchy is a derived
// artifact and is always paired with the source DEM stored by the same job.
// Child indices are reconstructed from parent pointers instead of persisted.
// A checksum and semantic checks reject partial or corrupted results.
std::vector<std::uint8_t> encodeDepressionHierarchy(const DepressionHierarchy &hierarchy);
DepressionHierarchy decodeDepressionHierarchy(const std::vector<std::uint8_t> &bytes,
                                              std::uint32_t expected_columns,
                                              std::uint32_t expected_rows);

} // namespace Aowis::Hydrology
#endif
