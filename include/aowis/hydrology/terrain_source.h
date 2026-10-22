#ifndef AOWIS_HYDROLOGY_TERRAIN_SOURCE_H
#define AOWIS_HYDROLOGY_TERRAIN_SOURCE_H

#include <aowis/hydrology/analysis.h>
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <atomic>
#include <cstdint>

namespace Aowis::Map { class TerrainData; }

namespace Aowis::Hydrology
{

struct SourceConfig
{
    // Map is the sole production source of terrain. In standalone builds the
    // embedding application supplies its existing Map TerrainData instance.
    QUrl map_url = QUrl(QStringLiteral("http://127.0.0.1:8122/"));
    QByteArray map_read_api_key; // optional existing Map read credential
    int request_timeout_ms = 180000;
    int patch_size = 256;
    int max_concurrent_requests = 4; // REST only; standalone Map remains sequential
#ifdef AOWIS_HYDROLOGY_LOCAL_MAP
    Aowis::Map::TerrainData *local_map = nullptr;
#endif
};

// Optional, fixed-size per-terrain-job diagnostics. Timings are monotonic wall
// times; map_request_ms includes Map computation, network transport and reply
// handling, not just TCP overhead. raster_copy_ms covers placing/decompressing
// float samples into the analysis raster. There is no unbounded per-patch log.
struct TerrainFetchMetrics
{
    QString transport; // "map_http" or "map_in_process"
    std::uint64_t patch_count = 0;
    std::uint64_t response_bytes = 0;
    double map_request_ms = 0.0; // summed lifetimes; can exceed elapsed fetch time
    double map_request_wall_ms = 0.0; // first request dispatch to last completion
    int peak_in_flight = 0;
    double raster_copy_ms = 0.0;
    double first_request_ms = 0.0;
    double fastest_request_ms = 0.0;
    double slowest_request_ms = 0.0;
};

// Uses Map's bounded source-precision raster contract for either transport.
// No direct Copernicus requests and no second DEM provider implementation.
// The optional metrics are local to this invocation; they are not thread-shared.
ElevationGrid readTerrain(const SourceConfig &source, const ElevationGrid &geometry,
                          const QString &dataset, const std::atomic<bool> &cancel_requested,
                          TerrainFetchMetrics *metrics = nullptr);

}
#endif
