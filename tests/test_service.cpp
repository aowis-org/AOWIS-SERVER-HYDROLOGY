#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/service.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <limits>

using namespace Aowis::Hydrology;

namespace
{
AnalysisRequest smallRequest()
{
    AnalysisRequest request;
    request.west = 8.0;
    request.east = 8.01;
    request.south = 50.0;
    request.north = 50.01;
    request.resolution_m = 30.0;
    return request;
}

void waitForState(const AnalysisService &service, const QString &id, const QString &state)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000)
    {
        if (service.job(id).value(QStringLiteral("state")).toString() == state)
            return;
        QThread::msleep(10);
    }
    throw std::runtime_error("Timed out waiting for job state: " + state.toStdString());
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    assert(temporary.isValid());
    ServiceConfig configuration;
    configuration.output_directory = temporary.path();
    configuration.maximum_cells = 100000;
    configuration.terrain_reader = [](const ElevationGrid &geometry, const QString &,
                                      const std::atomic<bool> &cancelled)
    {
        if (cancelled.load()) throw std::runtime_error("Job canceled");
        ElevationGrid result = geometry;
        result.elevations_m.resize(std::size_t(result.columns) * result.rows);
        for (std::uint32_t y = 0; y < result.rows; ++y)
            for (std::uint32_t x = 0; x < result.columns; ++x)
                result.elevations_m[std::size_t(y) * result.columns + x] =
                    float(1000 - int(y) - int(x));
        return result;
    };
    QString successful_id;
    {
        AnalysisService service(configuration);
        QString error;
        assert(service.initialize(&error));
        assert(!service.submit(AnalysisRequest(), &error).size());
        assert(!error.isEmpty());
        successful_id = service.submit(smallRequest(), &error);
        assert(!successful_id.isEmpty() && error.isEmpty());
        waitForState(service, successful_id, QStringLiteral("completed"));
        const QJsonObject completed = service.job(successful_id);
        assert(completed.value(QStringLiteral("horizontal_crs")) ==
            QStringLiteral("EPSG:4326"));
        assert(completed.value(QStringLiteral("vertical_datum")) ==
            QStringLiteral("EGM2008"));
        const int columns = completed.value(QStringLiteral("columns")).toInt();
        const int rows = completed.value(QStringLiteral("rows")).toInt();
        assert(columns > 1 && rows > 1);
        const QByteArray data = service.rasterWindow(successful_id, QStringLiteral("upstream-count"),
            0, 0, columns, rows, &error);
        assert(error.isEmpty() && data.size() == columns * rows * 8);
        const QByteArray fill_depth = service.rasterWindow(successful_id,
            QStringLiteral("potential-fill-depth"), 0, 0, columns, rows, &error);
        assert(error.isEmpty() && fill_depth.size() == columns * rows * 4);
        assert(completed.value(QStringLiteral("potential_fill_cells")).toDouble() == 0.0);
        assert(completed.value(QStringLiteral("potential_fill_volume_m3")).toDouble() == 0.0);
        assert(completed.value(QStringLiteral("accepted_breach_paths")).toInt() == 0);
        assert(completed.value(QStringLiteral("breached_cells")).toDouble() == 0.0);
        const QByteArray cut_depth = service.rasterWindow(successful_id,
            QStringLiteral("breach-cut-depth"), 0, 0, columns, rows, &error);
        assert(error.isEmpty() && cut_depth.size() == columns * rows * 4);
        AnalysisRequest invalid_breach = smallRequest();
        invalid_breach.breach_depressions = true;
        invalid_breach.maximum_breach_path_cells = 99;
        assert(service.submit(invalid_breach, &error).isEmpty());
        assert(error.contains(QStringLiteral("Invalid constrained breach")));
        const QByteArray mask = service.catchment(successful_id, columns - 1, rows - 1, &error);
        assert(error.isEmpty() && mask.size() == columns * rows);
        const QJsonObject watershed = service.watershedBoundary(
            successful_id, columns - 1, rows - 1, &error);
        assert(error.isEmpty());
        assert(watershed.value(QStringLiteral("type")) == QStringLiteral("Feature"));
        assert(watershed.value(QStringLiteral("geometry")).toObject()
            .value(QStringLiteral("type")) == QStringLiteral("MultiPolygon"));
        assert(watershed.value(QStringLiteral("properties")).toObject()
            .value(QStringLiteral("contributing_cells")).toDouble() > 0);
        const double west = completed.value(QStringLiteral("west")).toDouble();
        const double north = completed.value(QStringLiteral("north")).toDouble();
        const double lon_step = completed.value(QStringLiteral("step_lon_deg")).toDouble();
        const double lat_step = completed.value(QStringLiteral("step_lat_deg")).toDouble();
        const double x0 = west + lon_step;
        const double y0 = north - 2 * lat_step;
        const double x1 = west + 2 * lon_step;
        const double y1 = north - lat_step;
        const QJsonArray rainfall_ring{
            QJsonArray{x0, y0}, QJsonArray{x1, y0},
            QJsonArray{x1, y1}, QJsonArray{x0, y1}, QJsonArray{x0, y0}
        };
        const QJsonObject polygon_geometry{
            {QStringLiteral("type"), QStringLiteral("Polygon")},
            {QStringLiteral("coordinates"), QJsonArray{rainfall_ring}}
        };
        const QJsonObject polygon_query{
            {QStringLiteral("min_upstream_area_km2"), 0.001},
            {QStringLiteral("geometry"), polygon_geometry}
        };
        const QJsonObject affected = service.affectedNetwork(successful_id, polygon_query, &error);
        assert(error.isEmpty() && affected.value(QStringLiteral("type")) ==
            QStringLiteral("FeatureCollection"));
        assert(affected.value(QStringLiteral("source_cells")).toDouble() >= 1);
        assert(affected.value(QStringLiteral("downstream_affected_cells")).toDouble() >= 1);
        assert(affected.value(QStringLiteral("local_stream_order")).toBool());
        assert(service.affectedNetwork(successful_id, QJsonObject{}, &error).isEmpty());
        assert(error.startsWith(QStringLiteral("Invalid")));
        const QJsonObject trace = service.downstreamTrace(successful_id,
            1, 1, 20.0, &error);
        assert(error.isEmpty() && trace.value(QStringLiteral("type")) == QStringLiteral("Feature"));
        const QJsonObject trace_properties = trace.value(QStringLiteral("properties")).toObject();
        assert(trace_properties.value(QStringLiteral("traced_length_m")).toDouble() > 0);
        assert(trace_properties.value(QStringLiteral("points")).toArray().size() > 1);
        const QJsonObject network = service.channelNetwork(successful_id, 0.001, &error);
        assert(error.isEmpty() && network.value(QStringLiteral("type")) ==
            QStringLiteral("FeatureCollection"));
        assert(network.value(QStringLiteral("selected_cells")).toDouble() > 0);
        assert(!network.value(QStringLiteral("features")).toArray().isEmpty());
        const DrainageWindow network_window{west, north - rows * lat_step,
                                            west + columns * lon_step, north};
        const QJsonObject windowed_network = service.channelNetworkWindow(
            successful_id, 0.001, network_window, &error);
        assert(error.isEmpty());
        assert(windowed_network.value(QStringLiteral("type")) ==
            QStringLiteral("FeatureCollection"));
        assert(windowed_network.value(QStringLiteral("window_clipped")).toBool());
        assert(service.downstreamTrace(successful_id, -1, 0, 20.0, &error).isEmpty());
        assert(error.startsWith(QStringLiteral("Invalid")));
        assert(service.channelNetwork(successful_id, 0.0, &error).isEmpty());
        assert(error.startsWith(QStringLiteral("Invalid")));
        assert(!service.jobs().isEmpty());
        assert(service.cancel(successful_id)); // completed jobs remain completed
        assert(service.job(successful_id).value(QStringLiteral("state")) ==
            QStringLiteral("completed"));
    }
    // Metadata and binary results remain available across service restarts.
    {
        AnalysisService restored(configuration);
        QString error;
        assert(restored.initialize(&error));
        assert(restored.job(successful_id).value(QStringLiteral("state")) ==
            QStringLiteral("completed"));
        assert(restored.job(successful_id).value(QStringLiteral("vertical_datum")) ==
            QStringLiteral("EGM2008"));
        assert(restored.job(successful_id).value(QStringLiteral("breach_depressions")) == false);
        assert(restored.rasterWindow(successful_id, QStringLiteral("breach-cut-depth"),
            0, 0, 2, 2, &error).size() == 16);
        assert(restored.rasterWindow(successful_id, QStringLiteral("elevation"), 0, 0, 2, 2,
            &error).size() == 16);
        assert(error.isEmpty());
        const QJsonObject persisted_watershed = restored.watershedBoundary(
            successful_id, 1, 1, &error);
        assert(error.isEmpty() && !persisted_watershed.isEmpty());
        const QJsonObject persisted_trace = restored.downstreamTrace(
            successful_id, 1, 1, 20.0, &error);
        assert(error.isEmpty() && !persisted_trace.isEmpty());
        const QJsonObject persisted_network = restored.channelNetwork(
            successful_id, 0.001, &error);
        assert(error.isEmpty() && !persisted_network.isEmpty());
    }
    // An incomplete raster must be rejected even if its only missing cell is
    // at the edge; otherwise a false drainage outlet can appear there.
    configuration.terrain_reader = [](const ElevationGrid &geometry, const QString &,
                                      const std::atomic<bool> &) -> ElevationGrid
    {
        ElevationGrid grid = geometry;
        grid.elevations_m.assign(std::size_t(grid.columns) * grid.rows, 100.0f);
        grid.elevations_m[0] = std::numeric_limits<float>::quiet_NaN();
        return grid;
    };
    {
        AnalysisService service(configuration);
        QString error;
        assert(service.initialize(&error));
        const QString id = service.submit(smallRequest(), &error);
        assert(!id.isEmpty());
        waitForState(service, id, QStringLiteral("failed"));
        assert(service.job(id).value(QStringLiteral("error")).toString()
               .contains(QStringLiteral("DEM coverage incomplete")));
    }

    // Inject a provider that can be canceled while the worker is executing.
    configuration.terrain_reader = [](const ElevationGrid &, const QString &,
                                      const std::atomic<bool> &cancelled) -> ElevationGrid
    {
        while (!cancelled.load()) QThread::msleep(5);
        throw std::runtime_error("Job canceled");
    };
    {
        AnalysisService service(configuration);
        QString error;
        assert(service.initialize(&error));
        const QString id = service.submit(smallRequest(), &error);
        assert(!id.isEmpty());
        assert(service.cancel(id));
        waitForState(service, id, QStringLiteral("canceled"));
        assert(service.job(id).value(QStringLiteral("state")) == QStringLiteral("canceled"));
    }
    return 0;
}
