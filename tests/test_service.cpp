#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/service.h>

#include <QCoreApplication>
#include <QtEndian>
#include <cstring>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
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
    // Separate per-job output directories and worker-local terrain readers
    // permit bounded concurrent analysis. Verify the configured pool really
    // overlaps two jobs, does not admit a third, and can cancel a queued job.
    {
        ServiceConfig invalid = configuration;
        invalid.maximum_cells = 0;
        AnalysisService invalid_cells(invalid);
        QString error;
        assert(!invalid_cells.initialize(&error));
        assert(error.contains(QStringLiteral("max_cells")));
        invalid = configuration;
        invalid.max_active_terrain_cells = 3;
        AnalysisService invalid_cell_budget(invalid);
        assert(!invalid_cell_budget.initialize(&error));
        assert(error.contains(QStringLiteral("max_active_terrain_cells")));
        invalid = configuration;
        invalid.max_concurrent_analyses = 0;
        AnalysisService invalid_service(invalid);
        assert(!invalid_service.initialize(&error));
        assert(error.contains(QStringLiteral("max_concurrent_analyses")));
        invalid.max_concurrent_analyses = 5;
        AnalysisService oversized_service(invalid);
        assert(!oversized_service.initialize(&error));
        invalid = configuration;
        invalid.maximum_coupled_dem_cells = 3;
        AnalysisService invalid_coupled(invalid);
        assert(!invalid_coupled.initialize(&error));
        assert(error.contains(QStringLiteral("max_coupled_dem_cells")));
    }
    {
        // Same sized terrain analyses must share a *running-cell* budget.
        // Jobs remain admitted and FIFO-queued while the budget is held.
        QTemporaryDir budget_directory;
        assert(budget_directory.isValid());
        ServiceConfig limited = configuration;
        limited.output_directory = budget_directory.path();
        limited.max_concurrent_analyses = 2;
        limited.max_outstanding_analyses = 3;
        limited.max_active_terrain_cells = 912; // smallRequest() is 24 x 38
        const TerrainReader reader = configuration.terrain_reader;
        std::mutex lock;
        std::condition_variable ready;
        bool release = false;
        int started = 0;
        int active = 0;
        int peak = 0;
        limited.terrain_reader = [&](const ElevationGrid &grid, const QString &dataset,
                                     const std::atomic<bool> &cancelled)
        {
            {
                std::unique_lock<std::mutex> guard(lock);
                ++started;
                ++active;
                peak = std::max(peak, active);
                ready.notify_all();
                if (!ready.wait_for(guard, std::chrono::seconds(5), [&] { return release; }))
                    throw std::runtime_error("Terrain memory-budget test timed out");
                --active;
            }
            return reader(grid, dataset, cancelled);
        };
        AnalysisService service(limited);
        QString error;
        assert(service.initialize(&error));
        const QString first = service.submit(smallRequest(), &error);
        assert(!first.isEmpty());
        {
            std::unique_lock<std::mutex> guard(lock);
            assert(ready.wait_for(guard, std::chrono::seconds(5), [&] { return started == 1; }));
        }
        assert(service.terrainJobCapacity().value(QStringLiteral("active_cells")).toInt() == 912);
        const QString second = service.submit(smallRequest(), &error);
        assert(!second.isEmpty());
        assert(service.job(second).value(QStringLiteral("state")) == QStringLiteral("queued"));
        assert(service.terrainJobCapacity().value(QStringLiteral("active_cell_limit")).toInt() == 912);
        QThread::msleep(40);
        {
            std::lock_guard<std::mutex> guard(lock);
            assert(started == 1 && peak == 1);
            release = true;
        }
        ready.notify_all();
        waitForState(service, first, QStringLiteral("completed"));
        waitForState(service, second, QStringLiteral("completed"));
        assert(started == 2 && peak == 1);
        // A grid larger than the budget must be rejected before disk admission.
        ServiceConfig impossible = configuration;
        impossible.output_directory = budget_directory.path();
        impossible.max_active_terrain_cells = 100;
        AnalysisService rejected(impossible);
        assert(rejected.initialize(&error));
        assert(rejected.submit(smallRequest(), &error).isEmpty());
        assert(error.contains(QStringLiteral("active terrain cell budget")));
        assert(rejected.terrainJobCapacity().value(QStringLiteral("outstanding")).toInt() == 0);
    }
    {
        QTemporaryDir concurrent_directory;
        assert(concurrent_directory.isValid());
        ServiceConfig parallel = configuration;
        parallel.output_directory = concurrent_directory.path();
        parallel.max_concurrent_analyses = 2;
        parallel.max_outstanding_analyses = 3;
        const TerrainReader base_reader = configuration.terrain_reader;
        std::mutex gate_mutex;
        std::condition_variable gate_condition;
        int active = 0;
        int entered = 0;
        int peak = 0;
        bool release = false;
        parallel.terrain_reader = [&](const ElevationGrid &grid, const QString &dataset,
                                      const std::atomic<bool> &cancelled)
        {
            {
                std::unique_lock<std::mutex> lock(gate_mutex);
                ++active;
                ++entered;
                peak = std::max(peak, active);
                gate_condition.notify_all();
                if (!gate_condition.wait_for(lock, std::chrono::seconds(5),
                                             [&] { return release; }))
                    throw std::runtime_error("Concurrent workers did not release");
                --active;
            }
            return base_reader(grid, dataset, cancelled);
        };
        AnalysisService service(parallel);
        QString error;
        assert(service.initialize(&error));
        const QString first = service.submit(smallRequest(), &error);
        const QString second = service.submit(smallRequest(), &error);
        assert(!first.isEmpty() && !second.isEmpty() && first != second && error.isEmpty());
        {
            std::unique_lock<std::mutex> lock(gate_mutex);
            assert(gate_condition.wait_for(lock, std::chrono::seconds(5),
                                           [&] { return entered == 2; }));
            assert(peak == 2);
        }
        const QString third = service.submit(smallRequest(), &error);
        assert(!third.isEmpty() && error.isEmpty());
        const QJsonObject full_capacity = service.terrainJobCapacity();
        assert(full_capacity.value(QStringLiteral("running")).toInt() == 2);
        assert(full_capacity.value(QStringLiteral("queued")).toInt() == 1);
        assert(full_capacity.value(QStringLiteral("outstanding")).toInt() == 3);
        assert(service.submit(smallRequest(), &error).isEmpty());
        assert(error.contains(QStringLiteral("capacity reached")));
        QThread::msleep(35);
        {
            std::lock_guard<std::mutex> lock(gate_mutex);
            assert(entered == 2); // bounded pool, no third terrain reader
        }
        assert(service.cancel(third));
        waitForState(service, third, QStringLiteral("canceled"));
        // A job canceled before dispatch never starts a worker, so it has no
        // queue_wait timing (that field measures time until worker start).
        const QJsonObject canceled_timings = service.job(third)
            .value(QStringLiteral("stage_timings_ms")).toObject();
        assert(!canceled_timings.contains(QStringLiteral("queue_wait")));
        assert(service.terrainJobCapacity().value(QStringLiteral("outstanding")).toInt() == 2);

        // Canceling a pending job immediately frees admission. Keep this
        // replacement queued long enough to measure its actual wait time.
        const QString fourth = service.submit(smallRequest(), &error);
        assert(!fourth.isEmpty() && error.isEmpty());
        assert(service.job(fourth).value(QStringLiteral("state")) == QStringLiteral("queued"));
        QThread::msleep(35);
        {
            std::lock_guard<std::mutex> lock(gate_mutex);
            assert(entered == 2);
            release = true;
        }
        gate_condition.notify_all();
        waitForState(service, first, QStringLiteral("completed"));
        waitForState(service, second, QStringLiteral("completed"));
        waitForState(service, fourth, QStringLiteral("completed"));
        assert(peak == 2);
        const QJsonObject timings = service.job(fourth)
            .value(QStringLiteral("stage_timings_ms")).toObject();
        assert(timings.value(QStringLiteral("queue_wait")).toDouble(-1) >= 15.0);
        const QByteArray a = service.rasterWindow(first, QStringLiteral("elevation"),
                                                  0, 0, 3, 3, &error);
        assert(error.isEmpty() && !a.isEmpty());
        const QByteArray b = service.rasterWindow(second, QStringLiteral("elevation"),
                                                  0, 0, 3, 3, &error);
        assert(error.isEmpty() && a == b);
        const QByteArray c = service.rasterWindow(fourth, QStringLiteral("elevation"),
                                                  0, 0, 3, 3, &error);
        assert(error.isEmpty() && a == c);
    }
    QString successful_id;
    QJsonObject saved_job_timings;
    QString rainfall_scenario_id;
    QString coupled_simulation_id;
    {
        AnalysisService service(configuration);
        QString error;
        assert(service.initialize(&error));
        assert(!service.submit(AnalysisRequest(), &error).size());
        assert(!error.isEmpty());
        successful_id = service.submit(smallRequest(), &error);
        assert(!successful_id.isEmpty() && error.isEmpty());
        waitForState(service, successful_id, QStringLiteral("completed"));
        // Coupled routing is an independent persisted model: prescribed basin
        // inflows and surveyed hydraulic controls, NOT a derived flood forecast.
        const QJsonObject coupled_request{
            {QStringLiteral("reservoirs"), QJsonArray{QJsonObject{
                {QStringLiteral("component_index"), 10},
                {QStringLiteral("initial_storage_m3"), 1100.0},
                {QStringLiteral("stage_storage"), QJsonArray{
                    QJsonArray{0.0, 0.0}, QJsonArray{10.0, 1000.0},
                    QJsonArray{20.0, 2000.0}}}
            }}},
            {QStringLiteral("connections"), QJsonArray()},
            {QStringLiteral("outlets"), QJsonArray{QJsonObject{
                {QStringLiteral("component_index"), 10},
                {QStringLiteral("crest_elevation_m"), 10.0},
                {QStringLiteral("effective_width_m"), 2.0},
                {QStringLiteral("discharge_factor"), 0.9},
                {QStringLiteral("downstream_water_level_m"), 8.0}
            }}},
            {QStringLiteral("inflows"), QJsonArray{
                QJsonObject{{QStringLiteral("duration_seconds"), 0.1},
                    {QStringLiteral("volume_m3"), QJsonArray{10.0}}},
                QJsonObject{{QStringLiteral("duration_seconds"), 0.1},
                    {QStringLiteral("volume_m3"), QJsonArray{0.0}}}
            }},
            {QStringLiteral("options"), QJsonObject{
                {QStringLiteral("maximum_substep_seconds"), 0.01}}}
        };
        assert(service.startCoupledSimulation(successful_id,
            QJsonObject{}, &error).isEmpty());
        assert(error.startsWith(QStringLiteral("Invalid")));
        const QJsonObject queued = service.startCoupledSimulation(
            successful_id, coupled_request, &error);
        assert(error.isEmpty() && !queued.isEmpty());
        coupled_simulation_id = queued.value(QStringLiteral("simulation_id")).toString();
        assert(!coupled_simulation_id.isEmpty());
        QElapsedTimer coupled_timer;
        coupled_timer.start();
        bool coupled_completed = false;
        while (coupled_timer.elapsed() < 10000)
        {
            const QJsonObject state = service.coupledSimulationStatus(
                successful_id, coupled_simulation_id, &error);
            assert(error.isEmpty());
            if (state.value(QStringLiteral("state")) == QStringLiteral("completed"))
            {
                assert(state.value(QStringLiteral("result_ready")).toBool());
                coupled_completed = true;
                break;
            }
            assert(state.value(QStringLiteral("state")) != QStringLiteral("failed"));
            QThread::msleep(5);
        }
        assert(coupled_completed);
        const QJsonObject series = service.coupledSimulationSnapshots(
            successful_id, coupled_simulation_id, 0, 2, &error);
        assert(error.isEmpty());
        assert(series.value(QStringLiteral("total_snapshots")).toInt() == 3);
        assert(series.value(QStringLiteral("snapshots")).toArray().size() == 2);
        assert(series.value(QStringLiteral("has_more")).toBool());
        const QJsonObject last_page = service.coupledSimulationSnapshots(
            successful_id, coupled_simulation_id, 2, 2, &error);
        assert(error.isEmpty() && last_page.value(QStringLiteral("snapshots")).toArray().size() == 1);
        const QJsonObject last = last_page.value(QStringLiteral("snapshots")).toArray().first().toObject();
        assert(std::abs(last.value(QStringLiteral("total_storage_m3")).toDouble() +
            last.value(QStringLiteral("cumulative_boundary_outflow_m3")).toDouble() -
            1110.0) < 1e-6);
        assert(service.coupledSimulationSnapshots(successful_id, coupled_simulation_id,
            -1, 3, &error).isEmpty() && error.startsWith(QStringLiteral("Invalid")));
        // A client may specify hydraulic parameters, but it cannot fabricate
        // a DEM saddle junction for an internal weir control.
        QJsonObject bad_junction_request = coupled_request;
        bad_junction_request.insert(QStringLiteral("connections"), QJsonArray{QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("weir")},
            {QStringLiteral("junction_parent_index"), 99999},
            {QStringLiteral("first_component_index"), 10},
            {QStringLiteral("second_component_index"), 11},
            {QStringLiteral("crest_elevation_m"), 10.0},
            {QStringLiteral("effective_width_m"), 2.0},
            {QStringLiteral("discharge_factor"), 0.9},
            {QStringLiteral("submergence_rating"), QJsonArray{
                QJsonArray{0.0, 1.0}, QJsonArray{1.0, 0.0}}}
        }});
        const QJsonObject bad_started = service.startCoupledSimulation(
            successful_id, bad_junction_request, &error);
        assert(error.isEmpty() && !bad_started.isEmpty());
        const QString bad_id = bad_started.value(QStringLiteral("simulation_id")).toString();
        coupled_timer.restart();
        bool bad_finished = false;
        while (coupled_timer.elapsed() < 10000)
        {
            const QJsonObject bad_status = service.coupledSimulationStatus(
                successful_id, bad_id, &error);
            assert(error.isEmpty());
            if (bad_status.value(QStringLiteral("state")) == QStringLiteral("failed"))
            {
                bad_finished = true;
                assert(!bad_status.value(QStringLiteral("result_ready")).toBool());
                break;
            }
            QThread::msleep(5);
        }
        assert(bad_finished);
        const QJsonObject completed = service.job(successful_id);
        const QJsonObject timings = completed.value(QStringLiteral("stage_timings_ms")).toObject();
        // Custom terrain_reader callbacks have no Map patch timings to report.
        assert(!completed.contains(QStringLiteral("terrain_fetch_details")));
        const QStringList stage_names{
            QStringLiteral("terrain_fetch"), QStringLiteral("terrain_validation"),
            QStringLiteral("d8_analysis"), QStringLiteral("raster_persistence"),
            QStringLiteral("depression_hierarchy"), QStringLiteral("basin_persistence")};
        double measured_stage_sum = 0.0;
        for (const QString &name : stage_names)
        {
            assert(timings.contains(name));
            const double milliseconds = timings.value(name).toDouble(-1.0);
            assert(std::isfinite(milliseconds) && milliseconds >= 0.0);
            measured_stage_sum += milliseconds;
        }
        assert(timings.value(QStringLiteral("total_worker")).toDouble(-1.0) >=
            measured_stage_sum - 0.001);
        assert(timings.value(QStringLiteral("queue_wait")).toDouble(-1) >= 0.0);
        saved_job_timings = timings;
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
        // The memory-mapped D8 traversal must match the saved upstream-cell
        // totals for outlets across different locations, not only a corner.
        for (const auto &[ox, oy] : std::vector<std::pair<int, int>>{
                 {0, 0}, {1, 1}, {columns / 2, rows / 2}, {columns - 1, rows - 1}})
        {
            const QByteArray catchment_mask = service.catchment(successful_id, ox, oy, &error);
            assert(error.isEmpty() && catchment_mask.size() == columns * rows);
            const auto outlet = std::size_t(oy) * columns + ox;
            const quint64 expected = qFromLittleEndian<quint64>(data.constData() + outlet * 8);
            assert(catchment_mask.at(qsizetype(outlet)) == 1);
            assert(std::count(catchment_mask.cbegin(), catchment_mask.cend(), char(1)) ==
                   std::ptrdiff_t(expected));
        }
        // Async catchment must preserve exact bytes without holding a
        // long-running HTTP handler, including concurrent task polls.
        assert(service.startCatchment(successful_id, -1, 0, &error).isEmpty());
        assert(error.startsWith(QStringLiteral("Invalid")));
        const QJsonObject submitted_catchment = service.startCatchment(
            successful_id, columns - 1, rows - 1, &error);
        assert(error.isEmpty() && !submitted_catchment.isEmpty());
        const QString catchment_task_id = submitted_catchment.value(
            QStringLiteral("task_id")).toString();
        assert(!catchment_task_id.isEmpty());
        QElapsedTimer catchment_timer;
        catchment_timer.start();
        QJsonObject catchment_state;
        while (catchment_timer.elapsed() < 10000)
        {
            catchment_state = service.catchmentTask(successful_id, catchment_task_id, &error);
            assert(error.isEmpty() && !catchment_state.isEmpty());
            if (catchment_state.value(QStringLiteral("state")) == QStringLiteral("completed"))
                break;
            assert(catchment_state.value(QStringLiteral("state")) != QStringLiteral("failed"));
            QThread::msleep(10);
        }
        assert(catchment_state.value(QStringLiteral("state")) == QStringLiteral("completed"));
        const QByteArray async_mask = service.catchmentTaskMask(
            successful_id, catchment_task_id, &error);
        assert(error.isEmpty() && async_mask == mask);
        assert(service.cancelCatchmentTask(successful_id, catchment_task_id, &error));
        assert(service.catchmentTask(successful_id, catchment_task_id, &error)
            .value(QStringLiteral("state")) == QStringLiteral("completed"));
        assert(service.catchmentTask(successful_id, QStringLiteral("bad-id"), &error).isEmpty());
        assert(error.startsWith(QStringLiteral("Unknown")));
        // The oldest terminal task is evicted with its spool result when more
        // than four terminal tasks have been retained.
        for (int i = 0; i < 4; ++i)
        {
            const QJsonObject next = service.startCatchment(
                successful_id, columns - 1, rows - 1, &error);
            assert(error.isEmpty() && !next.isEmpty());
            const QString next_id = next.value(QStringLiteral("task_id")).toString();
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 10000)
            {
                const QJsonObject status = service.catchmentTask(successful_id, next_id, &error);
                assert(error.isEmpty());
                if (status.value(QStringLiteral("state")) == QStringLiteral("completed")) break;
                assert(status.value(QStringLiteral("state")) != QStringLiteral("failed"));
                QThread::msleep(10);
            }
            assert(service.catchmentTask(successful_id, next_id, &error)
                .value(QStringLiteral("state")) == QStringLiteral("completed"));
        }
        assert(service.catchmentTask(successful_id, catchment_task_id, &error).isEmpty());
        assert(error.startsWith(QStringLiteral("Unknown")));
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
        const QJsonObject rainfall_request{
            {QStringLiteral("name"), QStringLiteral("synthetic rain")},
            {QStringLiteral("geometry"), polygon_geometry},
            {QStringLiteral("depth_mm"), 25.0},
            {QStringLiteral("duration_seconds"), 3600.0},
            {QStringLiteral("effective_runoff_fraction"), 0.4}
        };
        const QJsonObject rainfall = service.createRainfallScenario(
            successful_id, rainfall_request, &error);
        assert(error.isEmpty());
        rainfall_scenario_id = rainfall.value(QStringLiteral("id")).toString();
        assert(!rainfall_scenario_id.isEmpty());
        assert(rainfall.value(QStringLiteral("wet_cells")).toInt() == 1);
        const double gross = rainfall.value(QStringLiteral("gross_rainfall_volume_m3")).toDouble();
        const double effective = rainfall.value(QStringLiteral("effective_runoff_volume_m3")).toDouble();
        assert(gross > 0.0);
        assert(std::abs(effective - gross * 0.4) < gross * 1.0e-6);
        assert(service.rainfallScenarios(successful_id, &error).size() == 1);
        assert(error.isEmpty());
        const QByteArray runoff_window = service.rainfallRunoffWindow(
            successful_id, rainfall_scenario_id, 1, 1, 1, 1, &error);
        assert(error.isEmpty() && runoff_window.size() == 8);
        const QJsonObject initially_pending = service.rainfallRedistributionStatus(
            successful_id, rainfall_scenario_id, &error);
        assert(error.isEmpty() && initially_pending.value(QStringLiteral("state")) ==
            QStringLiteral("not_started"));
        assert(!initially_pending.value(QStringLiteral("cache_ready")).toBool());
        assert(service.rainfallRedistributionReady(successful_id, rainfall_scenario_id,
            0, 20, &error).isEmpty());
        assert(error.startsWith(QStringLiteral("Redistribution result not ready")));
        const QJsonObject redistributed = service.rainfallRedistribution(
            successful_id, rainfall_scenario_id, 0, 20, &error);
        assert(error.isEmpty());
        assert(redistributed.value(QStringLiteral("model")) ==
            QStringLiteral("static_fill_spill_merge_volume_v1"));
        assert(redistributed.value(QStringLiteral("generated_runoff_m3")).toDouble() > 0);
        assert(std::abs(redistributed.value(QStringLiteral("generated_runoff_m3")).toDouble() -
            redistributed.value(QStringLiteral("basin_storage_m3")).toDouble() -
            redistributed.value(QStringLiteral("total_boundary_outflow_m3")).toDouble()) < 1.e-6);
        assert(redistributed.value(QStringLiteral("total_basins")).toDouble() == 0);
        assert(!redistributed.value(QStringLiteral("cache_hit")).toBool());
        assert(service.rainfallRedistributionStatus(successful_id, rainfall_scenario_id,
            &error).value(QStringLiteral("cache_ready")).toBool());
        assert(service.rainfallRedistributionReady(successful_id, rainfall_scenario_id,
            0, 20, &error).value(QStringLiteral("cache_hit")).toBool());
        assert(error.isEmpty());
        const QJsonObject cached_empty = service.rainfallRedistribution(
            successful_id, rainfall_scenario_id, 20, 1, &error);
        assert(error.isEmpty() && cached_empty.value(QStringLiteral("cache_hit")).toBool());
        assert(cached_empty.value(QStringLiteral("basins")).toArray().isEmpty());
        assert(service.rainfallRedistribution(successful_id, rainfall_scenario_id,
            -1, 20, &error).isEmpty());
        assert(error.startsWith(QStringLiteral("Invalid")));

        assert(service.createRainfallScenario(successful_id,
            QJsonObject{{QStringLiteral("geometry"), polygon_geometry}}, &error).isEmpty());
        assert(error.startsWith(QStringLiteral("Invalid")));
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
    // A completed terrain analysis can exceed the stricter coupled DEM
    // budget, but pure prescribed-inflow simulations need no raster loading.
    // Reject a DEM-dependent job before creating any simulation directory.
    {
        ServiceConfig limited_config = configuration;
        limited_config.maximum_coupled_dem_cells = 4;
        AnalysisService limited(limited_config);
        QString error;
        assert(limited.initialize(&error));
        const QJsonObject saved = limited.job(successful_id);
        assert(saved.value(QStringLiteral("state")) == QStringLiteral("completed"));
        assert(saved.value(QStringLiteral("columns")).toInt() *
               saved.value(QStringLiteral("rows")).toInt() > 4);
        const QString folder = QDir(configuration.output_directory).filePath(
            successful_id + QStringLiteral("/coupled-simulations"));
        const QStringList old_entries = QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        const QJsonObject dem_required{
            {QStringLiteral("connections"), QJsonArray{QJsonObject{}}},
            {QStringLiteral("reservoirs"), QJsonArray()}
        };
        assert(limited.startCoupledSimulation(successful_id, dem_required, &error).isEmpty());
        assert(error == QStringLiteral("Coupled DEM cell limit exceeded"));
        assert(QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot) == old_entries);
    }
    // Metadata and binary results remain available across service restarts.
    {
        AnalysisService restored(configuration);
        QString error;
        assert(restored.initialize(&error));
        assert(restored.job(successful_id).value(QStringLiteral("stage_timings_ms")).toObject() ==
            saved_job_timings);
        assert(restored.job(successful_id).value(QStringLiteral("state")) ==
            QStringLiteral("completed"));
        const QJsonObject persisted_coupled = restored.coupledSimulationStatus(
            successful_id, coupled_simulation_id, &error);
        assert(error.isEmpty());
        assert(persisted_coupled.value(QStringLiteral("state")) == QStringLiteral("completed"));
        const QJsonObject restored_series = restored.coupledSimulationSnapshots(
            successful_id, coupled_simulation_id, 1, 1, &error);
        assert(error.isEmpty() && restored_series.value(QStringLiteral("snapshots")).toArray().size() == 1);
        const QString series_path = QDir(configuration.output_directory).filePath(
            successful_id + QStringLiteral("/coupled-simulations/") +
            coupled_simulation_id + QStringLiteral("/series-v1.aowbin"));
        QFile damaged_series(series_path);
        assert(damaged_series.open(QIODevice::ReadWrite));
        assert(damaged_series.seek(64 + 32 + 8));
        const QByteArray previous_byte = damaged_series.read(1);
        assert(previous_byte.size() == 1);
        assert(damaged_series.seek(64 + 32 + 8));
        const char flipped = char(previous_byte.at(0) ^ 0x40);
        assert(damaged_series.write(&flipped, 1) == 1);
        damaged_series.close();
        assert(restored.coupledSimulationSnapshots(successful_id,
            coupled_simulation_id, 0, 1, &error).isEmpty());
        assert(error.contains(QStringLiteral("Corrupt coupled simulation snapshot")));

        assert(restored.job(successful_id).value(QStringLiteral("vertical_datum")) ==
            QStringLiteral("EGM2008"));
        assert(restored.job(successful_id).value(QStringLiteral("breach_depressions")) == false);
        assert(restored.rasterWindow(successful_id, QStringLiteral("breach-cut-depth"),
            0, 0, 2, 2, &error).size() == 16);
        assert(restored.rasterWindow(successful_id, QStringLiteral("elevation"), 0, 0, 2, 2,
            &error).size() == 16);
        assert(error.isEmpty());
        const QJsonObject saved_rain = restored.rainfallScenario(
            successful_id, rainfall_scenario_id, &error);
        assert(error.isEmpty());
        assert(saved_rain.value(QStringLiteral("name")) == QStringLiteral("synthetic rain"));
        assert(restored.rainfallScenarios(successful_id, &error).size() == 1);
        assert(restored.rainfallRunoffWindow(successful_id, rainfall_scenario_id,
            1, 1, 1, 1, &error).size() == 8);
        assert(error.isEmpty());
        const QJsonObject restored_balance = restored.rainfallRedistribution(
            successful_id, rainfall_scenario_id, 0, 100, &error);
        assert(error.isEmpty());
        assert(restored_balance.value(QStringLiteral("generated_runoff_m3")).toDouble() > 0);
        assert(restored_balance.value(QStringLiteral("cache_hit")).toBool());

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
    // A completed manifest is a publication promise: every required artifact
    // must survive a restart with exactly the expected shape and identity.
    // Exercise deletion, truncation, same-length silent corruption, damaged
    // hierarchy checksums, interrupted publication, and legacy metadata.
    {
        QTemporaryDir integrity_directory;
        assert(integrity_directory.isValid());
        ServiceConfig isolated = configuration;
        isolated.output_directory = integrity_directory.path();
        QString id;
        {
            AnalysisService service(isolated);
            QString error;
            assert(service.initialize(&error));
            id = service.submit(smallRequest(), &error);
            assert(!id.isEmpty() && error.isEmpty());
            waitForState(service, id, QStringLiteral("completed"));
            assert(service.job(id).value(QStringLiteral("artifact_integrity"))
                       .toObject().size() == 8);
        }
        const QDir folder(QDir(isolated.output_directory).filePath(id));
        const auto readBytes = [&](const QString &name)
        {
            QFile file(folder.filePath(name));
            assert(file.open(QIODevice::ReadOnly));
            return file.readAll();
        };
        const auto replaceBytes = [&](const QString &name, const QByteArray &bytes)
        {
            QFile file(folder.filePath(name));
            assert(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            assert(file.write(bytes) == bytes.size());
            file.close();
        };
        const QByteArray original_manifest = readBytes(QStringLiteral("job.json"));
        const QJsonObject original_job = QJsonDocument::fromJson(original_manifest).object();
        const auto checkStateAfterRestart = [&](const QString &expected,
                                                const QString &error_fragment)
        {
            AnalysisService service(isolated);
            QString error;
            assert(service.initialize(&error));
            const QJsonObject status = service.job(id);
            assert(status.value(QStringLiteral("state")) == expected);
            if (!error_fragment.isEmpty())
            {
                assert(status.value(QStringLiteral("error")).toString().contains(error_fragment));
                assert(service.rasterWindow(id, QStringLiteral("elevation"),
                    0, 0, 1, 1, &error).isEmpty());
            }
        };
        // Exactly one missing file must invalidate a previously complete job.
        const QString missing_name = QStringLiteral("conditioned.f32");
        const QByteArray original_conditioned = readBytes(missing_name);
        assert(QFile::remove(folder.filePath(missing_name)));
        checkStateAfterRestart(QStringLiteral("failed"), QStringLiteral("artifact"));
        replaceBytes(missing_name, original_conditioned);
        replaceBytes(QStringLiteral("job.json"), original_manifest);
        checkStateAfterRestart(QStringLiteral("completed"), {});

        // Correct name but truncated raster is still invalid.
        const QString truncated_name = QStringLiteral("downstream.i64");
        const QByteArray original_links = readBytes(truncated_name);
        replaceBytes(truncated_name, original_links.left(original_links.size() - 8));
        checkStateAfterRestart(QStringLiteral("failed"), QStringLiteral("length"));
        replaceBytes(truncated_name, original_links);
        replaceBytes(QStringLiteral("job.json"), original_manifest);

        // A same-size corruption cannot evade the new content identities.
        const QString corrupt_name = QStringLiteral("elevation.f32");
        const QByteArray original_elevation = readBytes(corrupt_name);
        QByteArray corrupted_elevation = original_elevation;
        corrupted_elevation[0] = char(corrupted_elevation.at(0) ^ 0x10);
        replaceBytes(corrupt_name, corrupted_elevation);
        checkStateAfterRestart(QStringLiteral("failed"), QStringLiteral("checksum"));
        replaceBytes(corrupt_name, original_elevation);
        replaceBytes(QStringLiteral("job.json"), original_manifest);

        const QString basin_name = QStringLiteral("basins.aowbin");
        const QByteArray original_basins = readBytes(basin_name);
        QByteArray corrupted_basins = original_basins;
        corrupted_basins[8] = char(corrupted_basins.at(8) ^ 0x01);
        replaceBytes(basin_name, corrupted_basins);
        checkStateAfterRestart(QStringLiteral("failed"), QStringLiteral("basin"));
        replaceBytes(basin_name, original_basins);
        replaceBytes(QStringLiteral("job.json"), original_manifest);

        QJsonObject broken_inventory = original_job;
        QJsonObject entries = original_job.value(QStringLiteral("artifact_integrity")).toObject();
        entries.remove(QStringLiteral("basins.aowbin"));
        broken_inventory.insert(QStringLiteral("artifact_integrity"), entries);
        replaceBytes(QStringLiteral("job.json"), QJsonDocument(broken_inventory).toJson());
        checkStateAfterRestart(QStringLiteral("failed"), QStringLiteral("inventory"));
        replaceBytes(QStringLiteral("job.json"), original_manifest);

        // Inventories are optional only for old completed analyses. Their
        // primary raster sizes and basin checksum are nevertheless checked.
        QJsonObject legacy = original_job;
        legacy.remove(QStringLiteral("artifact_integrity"));
        const QByteArray legacy_manifest = QJsonDocument(legacy).toJson();
        replaceBytes(QStringLiteral("job.json"), legacy_manifest);
        checkStateAfterRestart(QStringLiteral("completed"), {});
        replaceBytes(truncated_name, original_links.left(original_links.size() - 8));
        checkStateAfterRestart(QStringLiteral("failed"), QStringLiteral("length"));
        replaceBytes(truncated_name, original_links);
        replaceBytes(QStringLiteral("job.json"), original_manifest);

        // Interrupted status is terminal on restart even with intact files.
        QJsonObject interrupted = original_job;
        interrupted.insert(QStringLiteral("state"), QStringLiteral("saving"));
        replaceBytes(QStringLiteral("job.json"), QJsonDocument(interrupted).toJson());
        checkStateAfterRestart(QStringLiteral("failed"), QStringLiteral("interrupted"));
        replaceBytes(QStringLiteral("job.json"), original_manifest);
        checkStateAfterRestart(QStringLiteral("completed"), {});
    }
    // One closed, static basin persists across restarts. UUID identities are
    // stable, and level/volume samples use source elevations, not D8 heights.
    QString basin_job_id;
    QString basin_id;
    QString basin_scenario_id;
    QString scenario_coupled_simulation_id;
    configuration.terrain_reader = [](const ElevationGrid &geometry, const QString &,
                                      const std::atomic<bool> &)
    {
        ElevationGrid grid = geometry;
        grid.elevations_m.assign(std::size_t(grid.columns) * grid.rows, 15.0f);
        grid.elevations_m[std::size_t(grid.rows / 2) * grid.columns + grid.columns / 2] = 10.0f;
        return grid;
    };
    {
        AnalysisService service(configuration);
        QString error;
        assert(service.initialize(&error));
        basin_job_id = service.submit(smallRequest(), &error);
        assert(!basin_job_id.isEmpty() && error.isEmpty());
        waitForState(service, basin_job_id, QStringLiteral("completed"));
        assert(service.job(basin_job_id).value(QStringLiteral("basin_count")).toInt() == 1);
        const QJsonObject listing = service.basins(basin_job_id, 0, 1, &error);
        assert(error.isEmpty() && listing.value(QStringLiteral("total_basins")).toInt() == 1);
        assert(listing.value(QStringLiteral("basins")).toArray().size() == 1);
        const QJsonObject single = listing.value(QStringLiteral("basins")).toArray()[0].toObject();
        basin_id = single.value(QStringLiteral("id")).toString();
        assert(QUuid(QStringLiteral("{") + basin_id + QStringLiteral("}")).isNull() == false);
        assert(single.value(QStringLiteral("minimum_elevation_m")).toDouble() == 10.0);
        assert(single.value(QStringLiteral("spill_elevation_m")).toDouble() == 15.0);
        assert(single.value(QStringLiteral("capacity_at_spill_m3")).toDouble() > 0.0);
        const QJsonObject meta = service.job(basin_job_id);
        const int middle_x = meta.value(QStringLiteral("columns")).toInt() / 2;
        const int middle_y = meta.value(QStringLiteral("rows")).toInt() / 2;
        const double gx = meta.value(QStringLiteral("west")).toDouble();
        const double gy = meta.value(QStringLiteral("north")).toDouble();
        const double dx = meta.value(QStringLiteral("step_lon_deg")).toDouble();
        const double dy = meta.value(QStringLiteral("step_lat_deg")).toDouble();
        const double rx0 = gx + middle_x * dx;
        const double rx1 = rx0 + dx;
        const double ry1 = gy - middle_y * dy;
        const double ry0 = ry1 - dy;
        const QJsonObject centered_polygon{
            {QStringLiteral("type"), QStringLiteral("Polygon")},
            {QStringLiteral("coordinates"), QJsonArray{QJsonArray{
                QJsonArray{rx0, ry0}, QJsonArray{rx1, ry0},
                QJsonArray{rx1, ry1}, QJsonArray{rx0, ry1}, QJsonArray{rx0, ry0}}}}
        };
        const QJsonObject centered_rain{
            {QStringLiteral("geometry"), centered_polygon},
            {QStringLiteral("depth_mm"), 5.0},
            {QStringLiteral("duration_seconds"), 600.0},
            {QStringLiteral("effective_runoff_fraction"), 1.0}
        };
        const QJsonObject centered = service.createRainfallScenario(
            basin_job_id, centered_rain, &error);
        assert(error.isEmpty() && !centered.isEmpty());
        basin_scenario_id = centered.value(QStringLiteral("id")).toString();
        // Saved rainfall scenarios can now drive a separate, opt-in coupled
        // simulation. The reservoir curve remains EXPLICIT, not inferred.
        const double capacity = single.value(QStringLiteral("capacity_at_spill_m3")).toDouble();
        const QJsonObject forcing_reservoir{
            {QStringLiteral("component_index"), 0},
            {QStringLiteral("initial_storage_m3"), 0.0},
            {QStringLiteral("stage_storage"), QJsonArray{
                QJsonArray{10.0, 0.0}, QJsonArray{15.0, capacity},
                QJsonArray{20.0, capacity * 2.0}}}
        };
        const QJsonArray forcing_profile{
            QJsonObject{{QStringLiteral("duration_seconds"), 240.0},
                {QStringLiteral("runoff_fraction"), 0.25}},
            QJsonObject{{QStringLiteral("duration_seconds"), 360.0},
                {QStringLiteral("runoff_fraction"), 0.75}}
        };
        const QJsonObject forcing_request{
            {QStringLiteral("reservoirs"), QJsonArray{forcing_reservoir}},
            {QStringLiteral("connections"), QJsonArray()},
            {QStringLiteral("outlets"), QJsonArray()},
            {QStringLiteral("scenario_forcing"), QJsonObject{
                {QStringLiteral("scenario_id"), basin_scenario_id},
                {QStringLiteral("delivery_model"), QStringLiteral("instant")},
                {QStringLiteral("time_profile"), forcing_profile}}}
        };
        QJsonObject invalid_forcing = forcing_request;
        invalid_forcing.insert(QStringLiteral("inflows"), QJsonArray());
        assert(service.startCoupledSimulation(basin_job_id, invalid_forcing, &error).isEmpty());
        assert(error.contains(QStringLiteral("either scenario_forcing or inflows")));
        invalid_forcing = forcing_request;
        QJsonObject invalid_profile = invalid_forcing.value(
            QStringLiteral("scenario_forcing")).toObject();
        invalid_profile.insert(QStringLiteral("scenario_id"), QStringLiteral("not-a-uuid"));
        invalid_forcing.insert(QStringLiteral("scenario_forcing"), invalid_profile);
        assert(service.startCoupledSimulation(basin_job_id, invalid_forcing, &error).isEmpty());
        assert(error.contains(QStringLiteral("UUID")));
        for (const QString &delivery : {QStringLiteral("instant"),
                                        QStringLiteral("d8_constant_velocity")})
        {
            QJsonObject request = forcing_request;
            if (delivery == QStringLiteral("d8_constant_velocity"))
            {
                QJsonObject details = request.value(QStringLiteral("scenario_forcing")).toObject();
                details.insert(QStringLiteral("delivery_model"), delivery);
                details.insert(QStringLiteral("effective_velocity_m_per_s"), 0.5);
                details.insert(QStringLiteral("output_interval_seconds"), 60.0);
                request.insert(QStringLiteral("scenario_forcing"), details);
            }
            const QJsonObject started = service.startCoupledSimulation(basin_job_id, request, &error);
            assert(error.isEmpty());
            const QString run_id = started.value(QStringLiteral("simulation_id")).toString();
            assert(!run_id.isEmpty());
            if (delivery == QStringLiteral("instant"))
                scenario_coupled_simulation_id = run_id;
            QElapsedTimer timer;
            timer.start();
            QJsonObject finished;
            while (timer.elapsed() < 10000)
            {
                finished = service.coupledSimulationStatus(basin_job_id, run_id, &error);
                assert(error.isEmpty());
                if (finished.value(QStringLiteral("state")) == QStringLiteral("completed")) break;
                assert(finished.value(QStringLiteral("state")) != QStringLiteral("failed"));
                QThread::msleep(5);
            }
            assert(finished.value(QStringLiteral("state")) == QStringLiteral("completed"));
            const QJsonObject provenance = finished.value(
                QStringLiteral("scenario_forcing_result")).toObject();
            assert(provenance.value(QStringLiteral("scenario_id")).toString() == basin_scenario_id);
            assert(provenance.value(QStringLiteral("delivery_model")).toString() == delivery);
            assert(provenance.value(QStringLiteral("direct_dem_boundary_runoff_m3")).toDouble() < 1e-8);
            assert(provenance.value(QStringLiteral("delivered_to_basins_m3")).toDouble() > 0);
            assert(provenance.value(QStringLiteral("source_sha256")).toObject().size() == 4);
            const int total = service.coupledSimulationSnapshots(basin_job_id, run_id,
                0, 1, &error).value(QStringLiteral("total_snapshots")).toInt();
            assert(error.isEmpty());
            assert(total == (delivery == QStringLiteral("instant") ? 3 : 11));
            const QJsonObject end_page = service.coupledSimulationSnapshots(
                basin_job_id, run_id, total - 1, 1, &error);
            assert(error.isEmpty());
            const QJsonObject last = end_page.value(QStringLiteral("snapshots"))
                .toArray().first().toObject();
            assert(std::abs(last.value(QStringLiteral("cumulative_inflow_m3")).toDouble() -
                provenance.value(QStringLiteral("delivered_to_basins_m3")).toDouble()) < 1e-5);
            assert(std::abs(last.value(QStringLiteral("total_storage_m3")).toDouble() -
                provenance.value(QStringLiteral("delivered_to_basins_m3")).toDouble()) < 1e-5);
        }
        const QJsonObject basin_balance = service.rainfallRedistribution(
            basin_job_id, basin_scenario_id, 0, 1, &error);
        assert(error.isEmpty() && basin_balance.value(QStringLiteral("total_basins")).toInt() == 1);
        assert(basin_balance.value(QStringLiteral("basin_storage_m3")).toDouble() > 0);
        assert(!basin_balance.value(QStringLiteral("cache_hit")).toBool());
        const QJsonObject basin_page_repeat = service.rainfallRedistribution(
            basin_job_id, basin_scenario_id, 0, 1, &error);
        assert(error.isEmpty() && basin_page_repeat.value(QStringLiteral("cache_hit")).toBool());
        assert(basin_page_repeat.value(QStringLiteral("basins")) ==
               basin_balance.value(QStringLiteral("basins")));
        // Content-derived identity: merely changing a timestamp must NOT
        // invalidate the cache, while same-length edits with mtime restored
        // must not reuse a stale redistribution result.
        const QString basin_folder = QDir(configuration.output_directory).filePath(basin_job_id);
        const QString scenario_folder = QDir(basin_folder).filePath(
            QStringLiteral("rainfall-scenarios/") + basin_scenario_id);
        const QString manifest_path = QDir(scenario_folder).filePath(QStringLiteral("scenario.json"));
        QFile scenario_manifest(manifest_path);
        assert(scenario_manifest.open(QIODevice::ReadOnly));
        const QByteArray original_manifest = scenario_manifest.readAll();
        scenario_manifest.close();
        assert(QJsonDocument::fromJson(original_manifest).object()
            .value(QStringLiteral("artifact_integrity")).toObject().size() == 2);
        const QDateTime original_manifest_time = QFileInfo(manifest_path).lastModified();
        QByteArray altered_manifest = original_manifest;
        const QByteArray model_name("static_uniform_rainfall_runoff");
        const qsizetype model_pos = altered_manifest.indexOf(model_name);
        assert(model_pos >= 0);
        altered_manifest[model_pos] = 'S'; // Same length, valid JSON, no change to physics.
        QThread::msleep(2); // Distinct ctime even on coarse timestamp filesystems.
        assert(scenario_manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
        assert(scenario_manifest.write(altered_manifest) == altered_manifest.size());
        assert(scenario_manifest.setFileTime(original_manifest_time,
            QFileDevice::FileModificationTime));
        scenario_manifest.close();
        assert(QFileInfo(manifest_path).size() == original_manifest.size());
        const QJsonObject metadata_changed = service.rainfallRedistribution(
            basin_job_id, basin_scenario_id, 0, 1, &error);
        assert(error.isEmpty() && !metadata_changed.value(QStringLiteral("cache_hit")).toBool());
        assert(metadata_changed.value(QStringLiteral("basins")) ==
               basin_balance.value(QStringLiteral("basins")));
        assert(scenario_manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
        assert(scenario_manifest.write(original_manifest) == original_manifest.size());
        assert(scenario_manifest.setFileTime(original_manifest_time,
            QFileDevice::FileModificationTime));
        scenario_manifest.close();
        const QJsonObject original_rebuilt = service.rainfallRedistribution(
            basin_job_id, basin_scenario_id, 0, 1, &error);
        assert(error.isEmpty() && !original_rebuilt.value(QStringLiteral("cache_hit")).toBool());
        assert(original_rebuilt.value(QStringLiteral("basins")) ==
               basin_balance.value(QStringLiteral("basins")));

        const QString elevation_path = QDir(basin_folder).filePath(QStringLiteral("elevation.f32"));
        QFile elevation_file(elevation_path);
        assert(elevation_file.open(QIODevice::ReadWrite));
        assert(elevation_file.setFileTime(QFileInfo(elevation_path).lastModified().addSecs(5),
            QFileDevice::FileModificationTime));
        elevation_file.close();
        const QJsonObject touched_only = service.rainfallRedistribution(
            basin_job_id, basin_scenario_id, 0, 1, &error);
        assert(error.isEmpty() && touched_only.value(QStringLiteral("cache_hit")).toBool());

        const QString runoff_path = QDir(scenario_folder).filePath(QStringLiteral("runoff.f64"));
        const QDateTime original_runoff_time = QFileInfo(runoff_path).lastModified();
        QFile runoff_file(runoff_path);
        assert(runoff_file.open(QIODevice::ReadWrite));
        const QByteArray original_byte = runoff_file.read(1);
        assert(original_byte.size() == 1 && runoff_file.seek(0));
        QThread::msleep(2);
        const char changed_byte = char(original_byte.at(0) ^ 0x01);
        assert(runoff_file.write(&changed_byte, 1) == 1);
        assert(runoff_file.setFileTime(original_runoff_time,
            QFileDevice::FileModificationTime));
        runoff_file.close();
        const QJsonObject invalid_source = service.rainfallRedistribution(
            basin_job_id, basin_scenario_id, 0, 1, &error);
        assert(invalid_source.isEmpty() &&
               error.contains(QStringLiteral("SHA-256 mismatch")));
        QThread::msleep(2);
        assert(runoff_file.open(QIODevice::ReadWrite));
        assert(runoff_file.write(original_byte) == 1);
        assert(runoff_file.setFileTime(original_runoff_time,
            QFileDevice::FileModificationTime));
        runoff_file.close();
        const QJsonObject restored_source = service.rainfallRedistribution(
            basin_job_id, basin_scenario_id, 0, 1, &error);
        assert(error.isEmpty() && restored_source.value(QStringLiteral("cache_hit")).toBool());
        // Before the content-inventory schema, scenarios had only the runoff
        // raster and manifest. They must remain readable and rebuildable.
        QJsonObject legacy_scenario = QJsonDocument::fromJson(original_manifest).object();
        legacy_scenario.remove(QStringLiteral("artifact_integrity"));
        const QByteArray legacy_json = QJsonDocument(legacy_scenario).toJson(QJsonDocument::Compact);
        assert(scenario_manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
        assert(scenario_manifest.write(legacy_json) == legacy_json.size());
        scenario_manifest.close();
        const QJsonObject legacy_result = service.rainfallRedistribution(
            basin_job_id, basin_scenario_id, 0, 1, &error);
        assert(error.isEmpty() && !legacy_result.value(QStringLiteral("cache_hit")).toBool());
        assert(legacy_result.value(QStringLiteral("basins")) ==
               basin_balance.value(QStringLiteral("basins")));
        // Incomplete inventories must be rejected, not treated as legacy.
        legacy_scenario.insert(QStringLiteral("artifact_integrity"), QJsonObject{
            {QStringLiteral("runoff.f64"), QJsonObject{}}
        });
        const QByteArray incomplete_json = QJsonDocument(legacy_scenario).toJson(
            QJsonDocument::Compact);
        assert(scenario_manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
        assert(scenario_manifest.write(incomplete_json) == incomplete_json.size());
        scenario_manifest.close();
        assert(service.rainfallRedistribution(
            basin_job_id, basin_scenario_id, 0, 1, &error).isEmpty());
        assert(error.contains(QStringLiteral("Incomplete immutable")));
        assert(scenario_manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
        assert(scenario_manifest.write(original_manifest) == original_manifest.size());
        scenario_manifest.close();
        const QJsonObject restored_manifest = service.rainfallRedistribution(
            basin_job_id, basin_scenario_id, 0, 1, &error);
        assert(error.isEmpty() && !restored_manifest.value(QStringLiteral("cache_hit")).toBool());
        // An invalidated or damaged *derived* cache is automatically
        // recomputed from the persisted DEM + runoff + basin hierarchy.
        const QString cache_path = QDir(configuration.output_directory).filePath(
            basin_job_id + QStringLiteral("/rainfall-scenarios/") + basin_scenario_id +
            QStringLiteral("/redistribution-v1.aowbin"));
        QFile damaged(cache_path);
        assert(damaged.open(QIODevice::ReadWrite));
        assert(damaged.write("x", 1) == 1);
        damaged.close();
        const QJsonObject cache_repaired = service.rainfallRedistribution(
            basin_job_id, basin_scenario_id, 0, 1, &error);
        assert(error.isEmpty() && !cache_repaired.value(QStringLiteral("cache_hit")).toBool());
        assert(cache_repaired.value(QStringLiteral("basins")) ==
               basin_balance.value(QStringLiteral("basins")));
        assert(basin_balance.value(QStringLiteral("basins")).toArray().first().toObject()
            .value(QStringLiteral("basin_id")).toString() == basin_id);

        // A cache-only query cannot accidentally trigger a costly rebuild.
        assert(QFile::remove(cache_path));
        const QJsonObject not_ready = service.rainfallRedistributionStatus(
            basin_job_id, basin_scenario_id, &error);
        assert(error.isEmpty() && not_ready.value(QStringLiteral("state")) ==
            QStringLiteral("not_started"));
        assert(service.rainfallRedistributionReady(basin_job_id, basin_scenario_id,
            0, 1, &error).isEmpty());
        assert(error.startsWith(QStringLiteral("Redistribution result not ready")));
        const QJsonObject started = service.startRainfallRedistribution(
            basin_job_id, basin_scenario_id, &error);
        assert(error.isEmpty());
        const QString task_id = started.value(QStringLiteral("id")).toString();
        assert(!QUuid(QStringLiteral("{") + task_id + QStringLiteral("}")).isNull());
        // Repeated submissions are idempotent whether the task is still
        // queued, running, or has just published its cache.
        for (int attempt = 0; attempt < 32; ++attempt)
        {
            const QJsonObject duplicate = service.startRainfallRedistribution(
                basin_job_id, basin_scenario_id, &error);
            assert(error.isEmpty());
            assert(duplicate.value(QStringLiteral("id")).toString() == task_id);
        }
        QElapsedTimer redistribution_timer;
        redistribution_timer.start();
        bool async_complete = false;
        while (redistribution_timer.elapsed() < 10000)
        {
            const QJsonObject status = service.rainfallRedistributionStatus(
                basin_job_id, basin_scenario_id, &error);
            assert(error.isEmpty());
            if (status.value(QStringLiteral("state")) == QStringLiteral("completed"))
            {
                async_complete = true;
                break;
            }
            assert(status.value(QStringLiteral("state")) != QStringLiteral("failed"));
            QThread::msleep(5);
        }
        assert(async_complete);
        // Deterministic completed-task path: the committed archive is reused
        // together with its original task identity.
        const QJsonObject finished_redistribution = service.startRainfallRedistribution(
            basin_job_id, basin_scenario_id, &error);
        assert(error.isEmpty() && finished_redistribution.value(
            QStringLiteral("cache_ready")).toBool());
        assert(finished_redistribution.value(QStringLiteral("id")).toString() == task_id);
        const QJsonObject async_balance = service.rainfallRedistributionReady(
            basin_job_id, basin_scenario_id, 0, 1, &error);
        assert(error.isEmpty());
        assert(async_balance.value(QStringLiteral("cache_hit")).toBool());
        assert(async_balance.value(QStringLiteral("basins")) ==
            basin_balance.value(QStringLiteral("basins")));
        // A depth GET must not trigger reconstruction. The separate POST task
        // consumes the completed redistribution cache, not the runoff inputs.
        const int basin_columns = meta.value(QStringLiteral("columns")).toInt();
        const int basin_rows = meta.value(QStringLiteral("rows")).toInt();
        assert(service.staticInundationDepthWindow(basin_job_id, basin_scenario_id,
            middle_x, middle_y, 1, 1, &error).isEmpty());
        assert(error.startsWith(QStringLiteral("Inundation result not ready")));
        const QJsonObject initial_depth = service.staticInundationStatus(
            basin_job_id, basin_scenario_id, &error);
        assert(error.isEmpty() && initial_depth.value(QStringLiteral("state")) ==
            QStringLiteral("not_started"));
        const QJsonObject depth_task = service.startStaticInundation(
            basin_job_id, basin_scenario_id, &error);
        assert(error.isEmpty() && !depth_task.value(QStringLiteral("id")).toString().isEmpty());
        // The same UUID must survive the worker completing between the
        // initial cache probe and the task lookup in a duplicate POST.
        for (int attempt = 0; attempt < 32; ++attempt)
        {
            const QJsonObject depth_duplicate = service.startStaticInundation(
                basin_job_id, basin_scenario_id, &error);
            assert(error.isEmpty() && depth_duplicate.value(QStringLiteral("id")) ==
                depth_task.value(QStringLiteral("id")));
        }
        QElapsedTimer depth_timer;
        depth_timer.start();
        bool depth_complete = false;
        while (depth_timer.elapsed() < 10000)
        {
            const QJsonObject status = service.staticInundationStatus(
                basin_job_id, basin_scenario_id, &error);
            assert(error.isEmpty());
            if (status.value(QStringLiteral("state")) == QStringLiteral("completed"))
            {
                depth_complete = true;
                assert(status.value(QStringLiteral("inundation_status")) ==
                    QStringLiteral("available"));
                assert(status.value(QStringLiteral("depth_available")).toBool());
                assert(!status.value(QStringLiteral("allocation_assumed")).toBool());
                assert(status.value(QStringLiteral("surfaces")).toArray().size() == 1);
                assert(status.value(QStringLiteral("reconstructed_volume_m3")).toDouble() > 0.0);
                break;
            }
            assert(status.value(QStringLiteral("state")) != QStringLiteral("failed"));
            QThread::msleep(5);
        }
        assert(depth_complete);
        // A completed, valid inundation cache keeps the original UUID.
        const QJsonObject completed_depth = service.startStaticInundation(
            basin_job_id, basin_scenario_id, &error);
        assert(error.isEmpty() && completed_depth.value(QStringLiteral("cache_ready")).toBool());
        assert(completed_depth.value(QStringLiteral("id")) == depth_task.value(QStringLiteral("id")));
        const QByteArray depth_cell = service.staticInundationDepthWindow(
            basin_job_id, basin_scenario_id, middle_x, middle_y, 1, 1, &error);
        assert(error.isEmpty() && depth_cell.size() == 4);
        const quint32 depth_bits = qFromLittleEndian<quint32>(depth_cell.constData());
        float depth_value = 0.0f;
        std::memcpy(&depth_value, &depth_bits, sizeof(depth_value));
        assert(depth_value > 0.0f);
        const QByteArray whole_depth = service.staticInundationDepthWindow(
            basin_job_id, basin_scenario_id, 0, 0, basin_columns, basin_rows, &error);
        assert(error.isEmpty() && whole_depth.size() == basin_columns * basin_rows * 4);
        assert(service.staticInundationDepthWindow(basin_job_id, basin_scenario_id,
            basin_columns, 0, 1, 1, &error).isEmpty());
        assert(error.startsWith(QStringLiteral("Invalid")));
        assert(!service.cancelStaticInundation(basin_job_id, basin_scenario_id, &error));
        assert(error == QStringLiteral("No active inundation task"));

        // A corrupted row must never be returned. The manifest is discarded
        // and a subsequent POST regenerates the immutable derived result.
        const QString depth_path = QDir(configuration.output_directory).filePath(
            basin_job_id + QStringLiteral("/rainfall-scenarios/") + basin_scenario_id +
            QStringLiteral("/inundation-v2.f32"));
        QFile corrupt_depth(depth_path);
        assert(corrupt_depth.open(QIODevice::ReadWrite));
        assert(corrupt_depth.write("x", 1) == 1);
        corrupt_depth.close();
        assert(service.staticInundationDepthWindow(basin_job_id, basin_scenario_id,
            0, 0, 1, 1, &error).isEmpty());
        assert(error.contains(QStringLiteral("checksum")));
        const QJsonObject refresh = service.startStaticInundation(
            basin_job_id, basin_scenario_id, &error);
        assert(error.isEmpty() && !refresh.value(QStringLiteral("id")).toString().isEmpty());
        assert(refresh.value(QStringLiteral("id")) != depth_task.value(QStringLiteral("id")));
        depth_timer.restart();
        depth_complete = false;
        while (depth_timer.elapsed() < 10000)
        {
            const QJsonObject status = service.staticInundationStatus(
                basin_job_id, basin_scenario_id, &error);
            assert(error.isEmpty());
            if (status.value(QStringLiteral("state")) == QStringLiteral("completed"))
            {
                depth_complete = true;
                break;
            }
            assert(status.value(QStringLiteral("state")) != QStringLiteral("failed"));
            QThread::msleep(5);
        }
        assert(depth_complete);
        const QByteArray repaired = service.staticInundationDepthWindow(
            basin_job_id, basin_scenario_id, 0, 0, 1, 1, &error);
        assert(error.isEmpty() && repaired == whole_depth.left(4));
        assert(!service.cancelRainfallRedistribution(basin_job_id, basin_scenario_id, &error));
        assert(error == QStringLiteral("No active redistribution task"));
        assert(service.startRainfallRedistribution(basin_job_id, basin_scenario_id,
            &error).value(QStringLiteral("cache_ready")).toBool());
        assert(error.isEmpty());

        assert(service.basins(basin_job_id, 1, 1, &error)
               .value(QStringLiteral("basins")).toArray().isEmpty());
        assert(service.basins(basin_job_id, 0, 0, &error).isEmpty());
        assert(error.startsWith(QStringLiteral("Invalid")));
        const QJsonObject detail = service.basin(basin_job_id, basin_id, &error);
        assert(error.isEmpty() && detail.value(QStringLiteral("id")).toString() == basin_id);
        const QJsonObject storage = service.basinStorage(basin_job_id, basin_id, 12.0, &error);
        assert(error.isEmpty() && storage.value(QStringLiteral("water_volume_m3")).toDouble() > 0.0);
        const QJsonObject curve = service.basinStorageCurve(basin_job_id, basin_id, 5, &error);
        assert(error.isEmpty() && curve.value(QStringLiteral("samples")).toArray().size() == 5);
        assert(curve.value(QStringLiteral("samples")).toArray().last().toObject()
               .value(QStringLiteral("water_volume_m3")).toDouble() ==
               single.value(QStringLiteral("capacity_at_spill_m3")).toDouble());
        assert(service.basinStorage(basin_job_id, basin_id, 16.0, &error).isEmpty());
        assert(!error.isEmpty());
        assert(service.basin(basin_job_id, QStringLiteral("not-a-uuid"), &error).isEmpty());
        assert(error.startsWith(QStringLiteral("Invalid")));
    }
    {
        AnalysisService restored(configuration);
        QString error;
        assert(restored.initialize(&error));
        assert(restored.job(basin_job_id).value(QStringLiteral("state")) == QStringLiteral("completed"));
        const QJsonObject saved_forcing = restored.coupledSimulationStatus(
            basin_job_id, scenario_coupled_simulation_id, &error);
        assert(error.isEmpty() && saved_forcing.value(QStringLiteral("state")) ==
            QStringLiteral("completed"));
        assert(saved_forcing.value(QStringLiteral("scenario_forcing_result")).toObject()
            .value(QStringLiteral("scenario_id")).toString() == basin_scenario_id);
        const QJsonObject saved_forcing_page = restored.coupledSimulationSnapshots(
            basin_job_id, scenario_coupled_simulation_id, 2, 1, &error);
        assert(error.isEmpty() && saved_forcing_page.value(QStringLiteral("snapshots"))
            .toArray().size() == 1);
        const QJsonObject persisted = restored.basins(basin_job_id, 0, 10, &error);
        assert(error.isEmpty() && persisted.value(QStringLiteral("total_basins")).toInt() == 1);
        assert(persisted.value(QStringLiteral("basins")).toArray().first().toObject()
               .value(QStringLiteral("id")).toString() == basin_id);
        assert(!restored.basinStorageCurve(basin_job_id, basin_id, 3, &error).isEmpty());
        assert(error.isEmpty());
        const QJsonObject cached_after_restart = restored.rainfallRedistribution(
            basin_job_id, basin_scenario_id, 0, 1, &error);
        assert(error.isEmpty() && cached_after_restart.value(QStringLiteral("cache_hit")).toBool());
        assert(cached_after_restart.value(QStringLiteral("total_basins")).toInt() == 1);
        const QJsonObject restarted_depth = restored.staticInundationStatus(
            basin_job_id, basin_scenario_id, &error);
        assert(error.isEmpty() && restarted_depth.value(QStringLiteral("cache_ready")).toBool());
        assert(restarted_depth.value(QStringLiteral("inundation_status")) ==
            QStringLiteral("available"));
        const QByteArray post_restart_depth = restored.staticInundationDepthWindow(
            basin_job_id, basin_scenario_id, 0, 0, 1, 1, &error);
        assert(error.isEmpty() && post_restart_depth.size() == 4);
        const QJsonObject restarted_status = restored.rainfallRedistributionStatus(
            basin_job_id, basin_scenario_id, &error);
        assert(error.isEmpty() && restarted_status.value(QStringLiteral("cache_ready")).toBool());
        assert(restarted_status.value(QStringLiteral("state")) == QStringLiteral("completed"));
        assert(restarted_status.value(QStringLiteral("id")).isNull());
    }

    // Partially filled sibling depressions now return independent child ponds.
    // Their depths are explicitly conditional on the stored allocation, rather
    // than claiming an independently verified hydraulic equilibrium.
    configuration.terrain_reader = [](const ElevationGrid &geometry, const QString &,
                                      const std::atomic<bool> &) -> ElevationGrid
    {
        ElevationGrid grid = geometry;
        grid.elevations_m.assign(std::size_t(grid.columns) * grid.rows, 16.0f);
        const std::size_t y = grid.rows / 2;
        const std::size_t x = grid.columns / 2;
        assert(x > 4 && x + 4 < grid.columns && y > 2 && y + 2 < grid.rows);
        for (int dx = -1; dx <= 1; ++dx)
            grid.elevations_m[y * grid.columns + std::size_t(std::int64_t(x) + dx)] = 12.0f;
        grid.elevations_m[y * grid.columns + x - 2] = 10.0f;
        grid.elevations_m[y * grid.columns + x + 2] = 10.0f;
        return grid;
    };
    {
        AnalysisService service(configuration);
        QString error;
        assert(service.initialize(&error));
        const QString job_id = service.submit(smallRequest(), &error);
        assert(!job_id.isEmpty() && error.isEmpty());
        waitForState(service, job_id, QStringLiteral("completed"));
        const QJsonObject job_info = service.job(job_id);
        const int x = job_info.value(QStringLiteral("columns")).toInt() / 2 - 2;
        const int y = job_info.value(QStringLiteral("rows")).toInt() / 2;
        const double west = job_info.value(QStringLiteral("west")).toDouble() +
            x * job_info.value(QStringLiteral("step_lon_deg")).toDouble();
        const double east = west + job_info.value(QStringLiteral("step_lon_deg")).toDouble();
        const double north = job_info.value(QStringLiteral("north")).toDouble() -
            y * job_info.value(QStringLiteral("step_lat_deg")).toDouble();
        const double south = north - job_info.value(QStringLiteral("step_lat_deg")).toDouble();
        const QJsonObject partial_polygon{
            {QStringLiteral("type"), QStringLiteral("Polygon")},
            {QStringLiteral("coordinates"), QJsonArray{QJsonArray{
                QJsonArray{west, south}, QJsonArray{east, south},
                QJsonArray{east, north}, QJsonArray{west, north},
                QJsonArray{west, south}}}}
        };
        const QJsonObject rain{
            {QStringLiteral("geometry"), partial_polygon},
            {QStringLiteral("depth_mm"), 5.0},
            {QStringLiteral("duration_seconds"), 600.0},
            {QStringLiteral("effective_runoff_fraction"), 1.0}
        };
        const QJsonObject scenario = service.createRainfallScenario(job_id, rain, &error);
        assert(error.isEmpty() && !scenario.isEmpty());
        const QString scenario_id = scenario.value(QStringLiteral("id")).toString();
        const QJsonObject balance = service.rainfallRedistribution(
            job_id, scenario_id, 0, 1, &error);
        assert(error.isEmpty() && balance.value(QStringLiteral("basin_storage_m3")).toDouble() > 0.0);
        const QJsonObject started = service.startStaticInundation(job_id, scenario_id, &error);
        assert(error.isEmpty() && !started.isEmpty());
        QElapsedTimer timer;
        timer.start();
        bool complete = false;
        while (timer.elapsed() < 10000)
        {
            const QJsonObject status = service.staticInundationStatus(job_id, scenario_id, &error);
            assert(error.isEmpty());
            if (status.value(QStringLiteral("state")) == QStringLiteral("completed"))
            {
                assert(status.value(QStringLiteral("cache_ready")).toBool());
                assert(status.value(QStringLiteral("depth_available")).toBool());
                assert(status.value(QStringLiteral("allocation_assumed")).toBool());
                assert(status.value(QStringLiteral("inundation_status")) ==
                    QStringLiteral("available"));
                assert(!status.value(QStringLiteral("surfaces")).toArray().isEmpty());
                complete = true;
                break;
            }
            assert(status.value(QStringLiteral("state")) != QStringLiteral("failed"));
            QThread::msleep(5);
        }
        assert(complete);
        const QByteArray wet_sibling = service.staticInundationDepthWindow(
            job_id, scenario_id, x, y, 1, 1, &error);
        assert(error.isEmpty() && wet_sibling.size() == 4);
        const quint32 wet_bits = qFromLittleEndian<quint32>(wet_sibling.constData());
        float wet_depth = 0.0f;
        std::memcpy(&wet_depth, &wet_bits, sizeof(wet_depth));
        assert(wet_depth > 0.0f);
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

    // Asynchronous rainfall workers are bounded, share the same persistent
    // output format as synchronous callers, and do not race the 64-scenario
    // cap when several standalone threads create scenarios simultaneously.
    {
        QTemporaryDir rainfall_dir;
        assert(rainfall_dir.isValid());
        ServiceConfig rainfall_config = configuration;
        rainfall_config.output_directory = rainfall_dir.path();
        rainfall_config.terrain_reader = [](const ElevationGrid &geometry, const QString &,
                                            const std::atomic<bool> &)
        {
            ElevationGrid grid = geometry;
            grid.elevations_m.assign(std::size_t(grid.columns) * grid.rows, 100.0f);
            return grid;
        };
        AnalysisService service(rainfall_config);
        QString error;
        assert(service.initialize(&error));
        const QString id = service.submit(smallRequest(), &error);
        assert(!id.isEmpty());
        waitForState(service, id, QStringLiteral("completed"));
        const QJsonObject job = service.job(id);
        const double west = job.value(QStringLiteral("west")).toDouble();
        const double north = job.value(QStringLiteral("north")).toDouble();
        const double step_x = job.value(QStringLiteral("step_lon_deg")).toDouble();
        const double step_y = job.value(QStringLiteral("step_lat_deg")).toDouble();
        const QJsonArray ring{
            QJsonArray{west + step_x, north - 2 * step_y},
            QJsonArray{west + 2 * step_x, north - 2 * step_y},
            QJsonArray{west + 2 * step_x, north - step_y},
            QJsonArray{west + step_x, north - step_y},
            QJsonArray{west + step_x, north - 2 * step_y}
        };
        const QJsonObject input{
            {QStringLiteral("geometry"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("Polygon")},
                {QStringLiteral("coordinates"), QJsonArray{ring}}
            }},
            {QStringLiteral("depth_mm"), 30.0},
            {QStringLiteral("duration_seconds"), 600.0},
            {QStringLiteral("effective_runoff_fraction"), 0.75}
        };
        assert(service.startRainfallScenario(id, QJsonObject{}, &error).isEmpty());
        assert(error.startsWith(QStringLiteral("Invalid")));
        const QJsonObject accepted = service.startRainfallScenario(id, input, &error);
        assert(error.isEmpty());
        const QString task_id = accepted.value(QStringLiteral("task_id")).toString();
        assert(!task_id.isEmpty());
        assert(accepted.value(QStringLiteral("state")) == QStringLiteral("queued"));
        QElapsedTimer timer;
        timer.start();
        QJsonObject final_task;
        while (timer.elapsed() < 10000)
        {
            final_task = service.rainfallScenarioTask(id, task_id, &error);
            assert(error.isEmpty());
            if (final_task.value(QStringLiteral("state")) == QStringLiteral("completed")) break;
            assert(final_task.value(QStringLiteral("state")) != QStringLiteral("failed"));
            QThread::msleep(5);
        }
        assert(final_task.value(QStringLiteral("state")) == QStringLiteral("completed"));
        const QString scenario_id = final_task.value(QStringLiteral("scenario")).toObject()
            .value(QStringLiteral("id")).toString();
        assert(!scenario_id.isEmpty());
        assert(service.rainfallScenario(id, scenario_id, &error)
            .value(QStringLiteral("id")) == scenario_id);
        assert(service.cancelRainfallScenarioTask(id, task_id, &error));
        assert(service.rainfallScenarioTask(id, task_id, &error)
            .value(QStringLiteral("state")) == QStringLiteral("completed"));
        assert(service.rainfallScenarioTask(QStringLiteral("other"), task_id, &error).isEmpty());
        assert(error.contains(QStringLiteral("Unknown rainfall scenario task")));
        // An interrupted creation can leave a UUID directory without its
        // final manifest. It is not a committed scenario and must not consume
        // a quota slot on top of reservations held by active writers.
        const QString incomplete = QDir(rainfall_dir.path()).filePath(
            id + QStringLiteral("/rainfall-scenarios/") +
            QStringLiteral("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"));
        assert(QDir().mkpath(incomplete));
        assert(service.rainfallScenarios(id, &error).size() == 1 && error.isEmpty());
        std::atomic<int> succeeded {0};
        std::atomic<int> refused {0};
        std::vector<std::thread> clients;
        for (int client = 0; client < 12; ++client)
            clients.emplace_back([&]()
            {
                for (int attempt = 0; attempt < 6; ++attempt)
                {
                    QString local_error;
                    const QJsonObject scenario = service.createRainfallScenario(
                        id, input, &local_error);
                    if (!scenario.isEmpty())
                    {
                        assert(local_error.isEmpty());
                        ++succeeded;
                    }
                    else
                    {
                        assert(local_error.startsWith(QStringLiteral("Too many")));
                        ++refused;
                    }
                }
            });
        for (std::thread &client : clients) client.join();
        if (succeeded.load() != 63 || refused.load() != 9)
            fprintf(stderr, "Rainfall quota: succeeded=%d refused=%d (expected 63/9)\n",
                    succeeded.load(), refused.load());
        assert(succeeded == 63 && refused == 9);
        assert(service.rainfallScenarios(id, &error).size() == 64);
        assert(error.isEmpty());
        // The persisted ceiling applies to synchronous and asynchronous
        // callers alike, including after previously accepted workers finish.
        assert(service.createRainfallScenario(id, input, &error).isEmpty());
        assert(error.startsWith(QStringLiteral("Too many")));
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
