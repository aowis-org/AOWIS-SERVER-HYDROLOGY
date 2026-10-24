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
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Aowis::Hydrology
{
namespace
{
using Clock = std::chrono::steady_clock;

QString precise(double value)
{
    return QString::number(value, 'g', 17);
}

double elapsedMs(const Clock::time_point &start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct TerrainPatch
{
    std::uint32_t row = 0;
    std::uint32_t column = 0;
    int rows = 0;
    int columns = 0;
};

TerrainPatch patchAt(const ElevationGrid &geometry, const int patch_size,
                     const std::uint32_t row, const std::uint32_t column)
{
    return {row, column,
            int(std::min<std::uint32_t>(std::uint32_t(patch_size), geometry.rows - row)),
            int(std::min<std::uint32_t>(std::uint32_t(patch_size), geometry.columns - column))};
}

void recordRequest(TerrainFetchMetrics *metrics, const double duration_ms,
                   const std::uint64_t response_bytes, const QString &transport,
                   const std::uint64_t issue_number)
{
    if (metrics == nullptr) return;
    if (metrics->patch_count == 0)
    {
        metrics->transport = transport;
        metrics->fastest_request_ms = duration_ms;
        metrics->slowest_request_ms = duration_ms;
    }
    else
    {
        metrics->fastest_request_ms = std::min(metrics->fastest_request_ms, duration_ms);
        metrics->slowest_request_ms = std::max(metrics->slowest_request_ms, duration_ms);
    }
    if (issue_number == 0)
        metrics->first_request_ms = duration_ms;
    ++metrics->patch_count;
    metrics->response_bytes += response_bytes;
    // Sum of individual request lifetimes. With concurrency this is not
    // elapsed wall time: several requests may overlap.
    metrics->map_request_ms += duration_ms;
}

void copyRemotePatch(ElevationGrid &grid, const TerrainPatch &patch, const QByteArray &bytes)
{
    for (int local_row = 0; local_row < patch.rows; ++local_row)
    {
        const std::size_t target = std::size_t(patch.row + std::uint32_t(local_row)) *
            grid.columns + patch.column;
        for (int local_col = 0; local_col < patch.columns; ++local_col)
        {
            const qsizetype offset =
                (qsizetype(local_row) * patch.columns + local_col) * 4;
            const quint32 bits = qFromLittleEndian<quint32>(bytes.constData() + offset);
            float elevation = 0.0f;
            static_assert(sizeof(bits) == sizeof(elevation));
            std::memcpy(&elevation, &bits, sizeof(bits));
            grid.elevations_m[target + std::size_t(local_col)] = elevation;
        }
    }
}

QNetworkRequest makeRemoteRequest(const SourceConfig &source, const QString &dataset,
                                  const ElevationGrid &geometry, const TerrainPatch &patch)
{
    const double west = geometry.geometry.origin_x + patch.column * geometry.geometry.pixel_width;
    const double east = west + patch.columns * geometry.geometry.pixel_width;
    const double north = geometry.geometry.origin_y - patch.row * geometry.geometry.pixel_height;
    const double south = north - patch.rows * geometry.geometry.pixel_height;

    // QUrl::resolved() replaces the last path segment of a base without a
    // trailing slash ("http://host/map" -> "http://host/terrain/..."), which
    // silently drops a reverse-proxy mount prefix. Treat the base as a folder.
    QUrl base = source.map_url;
    if (!base.path().endsWith(QLatin1Char('/')))
        base.setPath(base.path() + QLatin1Char('/'));
    QUrl endpoint = base.resolved(QUrl(QStringLiteral("terrain/v1/analysis/grid")));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("dataset"), dataset);
    query.addQueryItem(QStringLiteral("west"), precise(west));
    query.addQueryItem(QStringLiteral("south"), precise(south));
    query.addQueryItem(QStringLiteral("east"), precise(east));
    query.addQueryItem(QStringLiteral("north"), precise(north));
    query.addQueryItem(QStringLiteral("columns"), QString::number(patch.columns));
    query.addQueryItem(QStringLiteral("rows"), QString::number(patch.rows));
    endpoint.setQuery(query);

    QNetworkRequest request(endpoint);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(source.request_timeout_ms);
    if (!source.map_read_api_key.isEmpty())
        request.setRawHeader("X-API-Key", source.map_read_api_key);
    return request;
}

void readRemoteTerrain(const SourceConfig &source, ElevationGrid &grid,
                       const QString &dataset, const std::atomic<bool> &cancel_requested,
                       TerrainFetchMetrics *metrics)
{
    // All replies, the event loop, and the network manager remain in the same
    // Hydrology worker thread. No Qt object is shared between jobs.
    QNetworkAccessManager manager;
    QEventLoop wait;
    QTimer cancellation_timer;
    cancellation_timer.setInterval(20);
    QString failure;
    std::vector<QNetworkReply *> active;
    active.reserve(std::size_t(source.max_concurrent_requests));
    std::uint32_t next_row = 0;
    std::uint32_t next_column = 0;
    std::uint64_t issued = 0;
    Clock::time_point first_issued{};
    Clock::time_point last_finished{};

    const auto hasMore = [&]() { return next_row < grid.rows; };
    const auto nextPatch = [&]()
    {
        const TerrainPatch patch = patchAt(grid, source.patch_size, next_row, next_column);
        next_column += std::uint32_t(source.patch_size);
        if (next_column >= grid.columns)
        {
            next_column = 0;
            next_row += std::uint32_t(source.patch_size);
        }
        return patch;
    };
    std::function<void()> dispatch;
    dispatch = [&]()
    {
        while (failure.isEmpty() && !cancel_requested.load() && hasMore() &&
               active.size() < std::size_t(source.max_concurrent_requests))
        {
            const TerrainPatch patch = nextPatch();
            const QNetworkRequest request = makeRemoteRequest(source, dataset, grid, patch);
            const Clock::time_point started = Clock::now();
            QNetworkReply *reply = manager.get(request);
            // A hard deadline is required in addition to Qt's transfer
            // timeout (which may reset when data continues arriving).
            QTimer *hard_timeout = new QTimer(reply);
            hard_timeout->setSingleShot(true);
            QObject::connect(hard_timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
            hard_timeout->start(source.request_timeout_ms);
            const std::uint64_t issue_number = issued++;
            if (issue_number == 0)
                first_issued = started;
            active.push_back(reply);
            if (metrics != nullptr)
                metrics->peak_in_flight = std::max(metrics->peak_in_flight,
                                                  int(active.size()));
            const auto finish = [&, reply, hard_timeout, started, patch, issue_number]()
            {
                if (std::find(active.begin(), active.end(), reply) == active.end())
                    return;
                hard_timeout->stop();
                const double request_ms = elapsedMs(started);
                last_finished = Clock::now();
                active.erase(std::remove(active.begin(), active.end(), reply), active.end());
                if (failure.isEmpty() && !cancel_requested.load())
                {
                    const int status = reply->attribute(
                        QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const QString mime = reply->header(
                        QNetworkRequest::ContentTypeHeader).toString();
                    const QNetworkReply::NetworkError error = reply->error();
                    const QString error_message = reply->errorString();
                    const QByteArray bytes = reply->readAll();
                    if (error != QNetworkReply::NoError || status != 200)
                        failure = QStringLiteral("Map terrain grid request failed (HTTP %1): %2 %3")
                            .arg(status).arg(error_message)
                            .arg(QString::fromUtf8(bytes.left(400)));
                    else if (!mime.startsWith(QStringLiteral("application/vnd.aowis.dem-f32le")))
                        failure = QStringLiteral("Map returned an unexpected DEM response content type");
                    else if (bytes.size() != qsizetype(patch.columns) * patch.rows * 4)
                        failure = QStringLiteral("Map returned a truncated or oversized DEM patch");
                    else
                    {
                        recordRequest(metrics, request_ms, std::uint64_t(bytes.size()),
                                      QStringLiteral("map_http"), issue_number);
                        const Clock::time_point copy_started = Clock::now();
                        copyRemotePatch(grid, patch, bytes);
                        if (metrics != nullptr)
                            metrics->raster_copy_ms += elapsedMs(copy_started);
                    }
                }
                reply->deleteLater();
                if (failure.isEmpty() && !cancel_requested.load())
                {
                    dispatch();
                    if (active.empty() && !hasMore())
                        wait.quit();
                }
                else
                    wait.quit();
            };
            QObject::connect(reply, &QNetworkReply::finished, &wait, finish);
            // Qt HTTP replies are normally asynchronous. Handle a reply that
            // was already completed (e.g. by a synchronous network backend).
            if (reply->isFinished())
                QTimer::singleShot(0, &wait, finish);
        }
    };

    QObject::connect(&cancellation_timer, &QTimer::timeout, &wait, [&]()
    {
        if (cancel_requested.load())
            wait.quit();
    });
    if (cancel_requested.load())
        throw std::runtime_error("Job canceled");
    dispatch();
    if (!active.empty() && failure.isEmpty() && !cancel_requested.load())
    {
        cancellation_timer.start();
        wait.exec();
        cancellation_timer.stop();
    }
    // Abort any outstanding operations before the manager or its replies are
    // destroyed; late completion slots must not issue additional patches.
    if (cancel_requested.load())
        failure = QStringLiteral("Job canceled");
    if (!failure.isEmpty())
    {
        // Prevent synchronous abort() completions from running the scheduler.
        const QString saved_failure = failure;
        for (QNetworkReply *reply : active)
            QObject::disconnect(reply, nullptr, &wait, nullptr);
        for (QNetworkReply *reply : active)
            reply->abort();
        throw std::runtime_error(saved_failure.toStdString());
    }
    if (metrics != nullptr && issued > 0)
        metrics->map_request_wall_ms = std::chrono::duration<double, std::milli>(
            last_finished - first_issued).count();
}
}

ElevationGrid readTerrain(const SourceConfig &source, const ElevationGrid &geometry,
                          const QString &dataset, const std::atomic<bool> &cancel_requested,
                          TerrainFetchMetrics *metrics)
{
    if (metrics != nullptr)
        *metrics = TerrainFetchMetrics{};
    if (geometry.geometry.horizontal_crs != HorizontalCrs::Wgs84Geographic)
        throw std::invalid_argument("Map terrain patch interface currently requires EPSG:4326");
    if (source.patch_size < 1 || source.patch_size > 512)
        throw std::invalid_argument("Source patch size must be in range 1..512");
    if (source.max_concurrent_requests < 1 || source.max_concurrent_requests > 4)
        throw std::invalid_argument("Map maximum concurrent requests must be in range 1..4");
    const std::uint64_t cells = std::uint64_t(geometry.columns) * geometry.rows;
    if (cells == 0 || cells > std::uint64_t(std::numeric_limits<int>::max()))
        throw std::invalid_argument("DEM grid too large for this job");
    ElevationGrid grid = geometry;
    grid.elevations_m.assign(std::size_t(cells), std::numeric_limits<float>::quiet_NaN());

#ifdef AOWIS_HYDROLOGY_LOCAL_MAP
    if (source.local_map != nullptr)
    {
        // The borrowed Map instance may have its own thread-affinity and
        // synchronization constraints. Keep its existing sequential contract.
        std::uint64_t issued = 0;
        for (std::uint32_t row = 0; row < geometry.rows;
             row += std::uint32_t(source.patch_size))
            for (std::uint32_t column = 0; column < geometry.columns;
                 column += std::uint32_t(source.patch_size))
            {
                if (cancel_requested.load())
                    throw std::runtime_error("Job canceled");
                const TerrainPatch patch = patchAt(geometry, source.patch_size, row, column);
                const double west = geometry.geometry.origin_x + column * geometry.geometry.pixel_width;
                const double east = west + patch.columns * geometry.geometry.pixel_width;
                const double north = geometry.geometry.origin_y - row * geometry.geometry.pixel_height;
                const double south = north - patch.rows * geometry.geometry.pixel_height;
                Aowis::Map::TerrainAnalysisGridRequest request;
                request.dataset = dataset;
                request.bounds = {west, south, east, north};
                request.columns = patch.columns;
                request.rows = patch.rows;
                const Clock::time_point request_started = Clock::now();
                const Aowis::Map::TerrainAnalysisGridResult result =
                    source.local_map->analysisGrid(request);
                if (result.status != Aowis::Map::TerrainProviderFetchStatus::Ready ||
                    result.elevations_m.size() != patch.columns * patch.rows)
                    throw std::runtime_error(result.error_message.toStdString());
                const double request_ms = elapsedMs(request_started);
                recordRequest(metrics, request_ms,
                    std::uint64_t(patch.columns) * std::uint64_t(patch.rows) * sizeof(float),
                    QStringLiteral("map_in_process"), issued++);
                const Clock::time_point copy_started = Clock::now();
                for (int local_row = 0; local_row < patch.rows; ++local_row)
                {
                    const std::size_t start = std::size_t(row + std::uint32_t(local_row)) *
                        geometry.columns + column;
                    for (int local_col = 0; local_col < patch.columns; ++local_col)
                        grid.elevations_m[start + std::size_t(local_col)] =
                            result.elevations_m[local_row * patch.columns + local_col];
                }
                if (metrics != nullptr)
                {
                    metrics->raster_copy_ms += elapsedMs(copy_started);
                    metrics->map_request_wall_ms += request_ms;
                    metrics->peak_in_flight = 1;
                }
            }
        return grid;
    }
#endif
    readRemoteTerrain(source, grid, dataset, cancel_requested, metrics);
    return grid;
}

} // namespace Aowis::Hydrology
