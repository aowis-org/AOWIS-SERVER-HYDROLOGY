#ifndef AOWIS_HYDROLOGY_GEOTIFF_SOURCE_H
#define AOWIS_HYDROLOGY_GEOTIFF_SOURCE_H

#include <aowis/hydrology/analysis.h>

#include <atomic>
#include <string>

namespace Aowis::Hydrology
{

// Reads a configured directory of local Copernicus-style DEM GeoTIFFs.
// Source filenames are NOT supplied by analysis clients. This function never
// downloads terrain, writes source files or changes the original elevations.
//
// Source rasters must be north-up WGS84 geographic grids with mutually
// matching pixel resolutions/alignment. Sampling onto a requested geographic
// grid uses nearest-neighbour (no interpolated/artificial elevation values).
// A missing source sample, even on the requested outer boundary, fails the
// request rather than treating gaps as drainage outlets.
//
// Throws std::invalid_argument for unsupported/malformed inputs and
// std::runtime_error for incomplete coverage, I/O or cancellation.
ElevationGrid readGeoTiffTerrain(const std::string &source_directory,
                                 const ElevationGrid &requested,
                                 const std::atomic<bool> *cancel_requested = nullptr);

} // namespace Aowis::Hydrology
#endif
