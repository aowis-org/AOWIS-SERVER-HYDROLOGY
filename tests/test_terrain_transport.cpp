#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/terrain_source.h>

#include <QCoreApplication>
#include <QHostAddress>
#include <QSharedPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QtEndian>

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace Aowis::Hydrology;

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTcpServer server;
    assert(server.listen(QHostAddress::LocalHost, 0));

    ElevationGrid geometry;
    geometry.columns = 7;
    geometry.rows = 5;
    geometry.geometry.horizontal_crs = HorizontalCrs::Wgs84Geographic;
    geometry.geometry.vertical_datum = VerticalDatum::Egm2008;
    geometry.geometry.origin_x = 8.5;
    geometry.geometry.origin_y = 50.25;
    geometry.geometry.pixel_width = 0.0001;
    geometry.geometry.pixel_height = 0.0001;

    int requests = 0;
    int connections = 0;
    std::vector<QString> patch_coordinates;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]()
    {
        while (server.hasPendingConnections())
        {
            QTcpSocket *socket = server.nextPendingConnection();
            ++connections;
            assert(socket != nullptr);
            const QSharedPointer<QByteArray> pending(new QByteArray);
            QObject::connect(socket, &QTcpSocket::readyRead, socket,
                [&, socket, pending]()
            {
                pending->append(socket->readAll());
                for (;;)
                {
                    const qsizetype end = pending->indexOf("\r\n\r\n");
                    if (end < 0) break;
                    const QByteArray header = pending->left(end + 4);
                    pending->remove(0, end + 4);
                    const QList<QByteArray> line = header.split('\n').first().trimmed().split(' ');
                    assert(line.size() >= 2);
                    assert(line.first() == "GET");
                    const QUrl requested(QString::fromUtf8(line[1]));
                    assert(requested.path() == QStringLiteral("/terrain/v1/analysis/grid"));
                    const QUrlQuery query(requested);
                    assert(query.queryItemValue(QStringLiteral("dataset")) == QStringLiteral("copernicus-glo30"));
                    const int width = query.queryItemValue(QStringLiteral("columns")).toInt();
                    const int height = query.queryItemValue(QStringLiteral("rows")).toInt();
                    const double west = query.queryItemValue(QStringLiteral("west")).toDouble();
                    const double north = query.queryItemValue(QStringLiteral("north")).toDouble();
                    const int x0 = int(std::lround((west - geometry.geometry.origin_x) /
                        geometry.geometry.pixel_width));
                    const int y0 = int(std::lround((geometry.geometry.origin_y - north) /
                        geometry.geometry.pixel_height));
                    assert(width >= 1 && width <= 3);
                    assert(height >= 1 && height <= 3);
                    assert(x0 >= 0 && y0 >= 0);
                    assert(x0 + width <= int(geometry.columns));
                    assert(y0 + height <= int(geometry.rows));
                    patch_coordinates.push_back(QStringLiteral("%1,%2:%3x%4")
                        .arg(x0).arg(y0).arg(width).arg(height));
                    ++requests;

                    QByteArray body(width * height * 4, '\0');
                    for (int row = 0; row < height; ++row)
                        for (int column = 0; column < width; ++column)
                        {
                            const float elevation = float(1000 + (y0 + row) * 100 + x0 + column);
                            std::uint32_t bits = 0;
                            std::memcpy(&bits, &elevation, sizeof(bits));
                            qToLittleEndian<quint32>(bits, body.data() +
                                (row * width + column) * 4);
                        }
                    const QByteArray response = QByteArray("HTTP/1.1 200 OK\r\n") +
                        "Content-Type: application/vnd.aowis.dem-f32le\r\n" +
                        "Content-Length: " + QByteArray::number(body.size()) + "\r\n" +
                        "Connection: keep-alive\r\n\r\n" + body;
                    socket->write(response);
                    socket->flush();
                }
            });
            QObject::connect(socket, &QTcpSocket::disconnected,
                socket, &QObject::deleteLater);
        }
    });

    SourceConfig source;
    source.map_url = QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
    source.patch_size = 3;
    source.request_timeout_ms = 5000;
    const std::atomic<bool> running{false};
    TerrainFetchMetrics metrics;
    const ElevationGrid loaded = readTerrain(source, geometry,
        QStringLiteral("copernicus-glo30"), running, &metrics);
    assert(metrics.transport == QStringLiteral("map_http"));
    assert(metrics.patch_count == 6);
    assert(metrics.response_bytes == 7ULL * 5ULL * sizeof(float));
    assert(std::isfinite(metrics.map_request_ms) && metrics.map_request_ms >= 0.0);
    assert(std::isfinite(metrics.raster_copy_ms) && metrics.raster_copy_ms >= 0.0);
    assert(metrics.fastest_request_ms <= metrics.first_request_ms);
    assert(metrics.first_request_ms <= metrics.slowest_request_ms);
    assert(metrics.slowest_request_ms <= metrics.map_request_ms + 0.001);
    assert(requests == 6); // 7x5 cells acquired in 3x3 source patches.
    // A persistent HTTP/1.1 server should see one connection per terrain job,
    // not a new TCP connection for each patch.
    assert(connections == 1);
    assert(patch_coordinates.size() == 6);
    assert(patch_coordinates[0] == QStringLiteral("0,0:3x3"));
    assert(patch_coordinates[1] == QStringLiteral("3,0:3x3"));
    assert(patch_coordinates[2] == QStringLiteral("6,0:1x3"));
    assert(patch_coordinates[3] == QStringLiteral("0,3:3x2"));
    assert(patch_coordinates[4] == QStringLiteral("3,3:3x2"));
    assert(patch_coordinates[5] == QStringLiteral("6,3:1x2"));
    assert(loaded.columns == geometry.columns && loaded.rows == geometry.rows);
    for (int row = 0; row < int(loaded.rows); ++row)
        for (int column = 0; column < int(loaded.columns); ++column)
            assert(loaded.elevations_m[std::size_t(row) * loaded.columns + column] ==
                float(1000 + row * 100 + column));

    // Cancellation remains effective before issuing the next network patch.
    const std::atomic<bool> cancelled{true};
    bool cancellation_caught = false;
    try
    {
        (void)readTerrain(source, geometry, QStringLiteral("copernicus-glo30"), cancelled);
    }
    catch (const std::runtime_error &error)
    {
        cancellation_caught = std::strcmp(error.what(), "Job canceled") == 0;
    }
    assert(cancellation_caught);
    assert(requests == 6);
    return 0;
}
