#ifndef AOWIS_HYDROLOGY_TERRAIN_SOURCE_H
#define AOWIS_HYDROLOGY_TERRAIN_SOURCE_H

#include <aowis/hydrology/analysis.h>
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <atomic>

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
#ifdef AOWIS_HYDROLOGY_LOCAL_MAP
    Aowis::Map::TerrainData *local_map = nullptr;
#endif
};

// Uses Map's bounded source-precision raster contract for either transport.
// No direct Copernicus requests and no second DEM provider implementation.
ElevationGrid readTerrain(const SourceConfig &source, const ElevationGrid &geometry,
                          const QString &dataset, const std::atomic<bool> &cancel_requested);

}
#endif
