#include <aowis/hydrology/terrain_source.h>

#ifdef AOWIS_HYDROLOGY_LOCAL_MAP
#include <aowis/map/terrain_data.h>
#endif

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace Aowis::Hydrology
{
namespace
{
QString precise(double value)
{
    return QString::number(value, 'g', 17);
}

QByteArray fetchRemoteGrid(QNetworkAccessManager &manager, const SourceConfig &source, const QString &dataset,
                           double west, double south, double east, double north,
                           int columns, int rows)
{
    QUrl endpoint = source.map_url.resolved(QUrl(QStringLiteral("terrain/v1/analysis/grid")));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("dataset"), dataset);
    query.addQueryItem(QStringLiteral("west"), precise(west));
    query.addQueryItem(QStringLiteral("south"), precise(south));
    query.addQueryItem(QStringLiteral("east"), precise(east));
    query.addQueryItem(QStringLiteral("north"), precise(north));
    query.addQueryItem(QStringLiteral("columns"), QString::number(columns));
    query.addQueryItem(QStringLiteral("rows"), QString::number(rows));
    endpoint.setQuery(query);

    QNetworkRequest request(endpoint);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(source.request_timeout_ms);
    if (!source.map_read_api_key.isEmpty())
        request.setRawHeader("X-API-Key", source.map_read_api_key);
    QNetworkReply *reply = manager.get(request);
    QEventLoop wait;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::finished, &wait, &QEventLoop::quit);
    timeout.start(source.request_timeout_ms);
    if (!reply->isFinished())
        wait.exec();
    timeout.stop();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString mime = reply->header(QNetworkRequest::ContentTypeHeader).toString();
    const QNetworkReply::NetworkError error = reply->error();
    const QString error_message = reply->errorString();
    QByteArray response = reply->readAll();
    reply->deleteLater();
    if (error != QNetworkReply::NoError || status != 200)
        throw std::runtime_error(QStringLiteral("Map terrain grid request failed (HTTP %1): %2 %3")
            .arg(status).arg(error_message).arg(QString::fromUtf8(response.left(400)))
            .toStdString());
    if (!mime.startsWith(QStringLiteral("application/vnd.aowis.dem-f32le")))
        throw std::runtime_error("Map returned an unexpected DEM response content type");
    const qsizetype expected = qsizetype(columns) * rows * 4;
    if (response.size() != expected)
        throw std::runtime_error("Map returned a truncated or oversized DEM patch");
    return response;
}
}

ElevationGrid readTerrain(const SourceConfig &source, const ElevationGrid &geometry,
                          const QString &dataset, const std::atomic<bool> &cancel_requested,
                          TerrainFetchMetrics *metrics)
{
    using Clock = std::chrono::steady_clock;
    if (metrics != nullptr)
        *metrics = TerrainFetchMetrics{};
    const auto elapsedMs = [](const Clock::time_point &start)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    const auto recordRequest = [metrics](const double duration_ms, const std::uint64_t response_bytes,
                                          const QString &transport)
    {
        if (metrics == nullptr) return;
        if (metrics->patch_count == 0)
        {
            metrics->transport = transport;
            metrics->first_request_ms = duration_ms;
            metrics->fastest_request_ms = duration_ms;
            metrics->slowest_request_ms = duration_ms;
        }
        else
        {
            metrics->fastest_request_ms = std::min(metrics->fastest_request_ms, duration_ms);
            metrics->slowest_request_ms = std::max(metrics->slowest_request_ms, duration_ms);
        }
        ++metrics->patch_count;
        metrics->response_bytes += response_bytes;
        metrics->map_request_ms += duration_ms;
    };
    if (geometry.geometry.horizontal_crs != HorizontalCrs::Wgs84Geographic)
        throw std::invalid_argument("Map terrain patch interface currently requires EPSG:4326");
    if (source.patch_size < 1 || source.patch_size > 512)
        throw std::invalid_argument("Source patch size must be in range 1..512");
    const std::uint64_t cells = std::uint64_t(geometry.columns) * geometry.rows;
    if (cells == 0 || cells > std::uint64_t(std::numeric_limits<int>::max()))
        throw std::invalid_argument("DEM grid too large for this job");
    ElevationGrid grid = geometry;
    grid.elevations_m.assign(std::size_t(cells), std::numeric_limits<float>::quiet_NaN());

    // One manager per terrain-read invocation: requests stay sequential and share
    // this worker thread, but Qt can reuse the HTTP connection across patches.
    // Never share a QNetworkAccessManager across concurrent worker threads.
    QNetworkAccessManager manager;

    for (std::uint32_t row = 0; row < geometry.rows; row += std::uint32_t(source.patch_size))
    {
        for (std::uint32_t column = 0; column < geometry.columns;
             column += std::uint32_t(source.patch_size))
        {
            if (cancel_requested.load())
                throw std::runtime_error("Job canceled");
            const int patch_rows = int(std::min<std::uint32_t>(
                std::uint32_t(source.patch_size), geometry.rows - row));
            const int patch_columns = int(std::min<std::uint32_t>(
                std::uint32_t(source.patch_size), geometry.columns - column));
            const double west = geometry.geometry.origin_x + column * geometry.geometry.pixel_width;
            const double east = west + patch_columns * geometry.geometry.pixel_width;
            const double north = geometry.geometry.origin_y - row * geometry.geometry.pixel_height;
            const double south = north - patch_rows * geometry.geometry.pixel_height;

#ifdef AOWIS_HYDROLOGY_LOCAL_MAP
            if (source.local_map != nullptr)
            {
                Aowis::Map::TerrainAnalysisGridRequest request;
                request.dataset = dataset;
                request.bounds = {west, south, east, north};
                request.columns = patch_columns;
                request.rows = patch_rows;
                const Clock::time_point request_started = Clock::now();
                const Aowis::Map::TerrainAnalysisGridResult result =
                    source.local_map->analysisGrid(request);
                if (result.status != Aowis::Map::TerrainProviderFetchStatus::Ready ||
                    result.elevations_m.size() != patch_columns * patch_rows)
                    throw std::runtime_error(result.error_message.toStdString());
                recordRequest(elapsedMs(request_started),
                    std::uint64_t(patch_columns) * std::uint64_t(patch_rows) * sizeof(float),
                    QStringLiteral("map_in_process"));
                const Clock::time_point copy_started = Clock::now();
                for (int local_row = 0; local_row < patch_rows; ++local_row)
                {
                    const std::size_t start = std::size_t(row + local_row) * geometry.columns + column;
                    for (int local_col = 0; local_col < patch_columns; ++local_col)
                        grid.elevations_m[start + std::size_t(local_col)] =
                            result.elevations_m[local_row * patch_columns + local_col];
                }
                if (metrics != nullptr)
                    metrics->raster_copy_ms += elapsedMs(copy_started);
                continue;
            }
#endif
            const Clock::time_point request_started = Clock::now();
            const QByteArray bytes = fetchRemoteGrid(
                manager, source, dataset, west, south, east, north, patch_columns, patch_rows);
            recordRequest(elapsedMs(request_started), std::uint64_t(bytes.size()),
                QStringLiteral("map_http"));
            const Clock::time_point copy_started = Clock::now();
            for (int local_row = 0; local_row < patch_rows; ++local_row)
            {
                const std::size_t target = std::size_t(row + local_row) * geometry.columns + column;
                for (int local_col = 0; local_col < patch_columns; ++local_col)
                {
                    const qsizetype offset = (qsizetype(local_row) * patch_columns + local_col) * 4;
                    const quint32 bits = qFromLittleEndian<quint32>(bytes.constData() + offset);
                    float elevation = 0.0f;
                    static_assert(sizeof(bits) == sizeof(elevation));
                    std::memcpy(&elevation, &bits, sizeof(bits));
                    grid.elevations_m[target + std::size_t(local_col)] = elevation;
                }
            }
            if (metrics != nullptr)
                metrics->raster_copy_ms += elapsedMs(copy_started);
        }
    }
    return grid;
}

} // namespace Aowis::Hydrology
