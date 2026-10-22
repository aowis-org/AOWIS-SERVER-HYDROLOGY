#ifndef AOWIS_HYDROLOGY_SERVICE_H
#define AOWIS_HYDROLOGY_SERVICE_H

#include <aowis/hydrology/analysis.h>
#include <aowis/hydrology/flow_network.h>
#include <aowis/hydrology/rainfall.h>
#include <aowis/hydrology/spatial_queries.h>
#include <aowis/hydrology/terrain_source.h>

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QSharedPointer>
#include <QString>
#include <QThreadPool>

#include <atomic>
#include <cstdint>
#include <functional>

namespace Aowis::Hydrology
{

struct AnalysisRequest
{
    QString dataset;
    double west = 0.0;
    double south = 0.0;
    double east = 0.0;
    double north = 0.0;
    double resolution_m = 30.0;
    bool fill_depressions = true;
    bool breach_depressions = false;
    double maximum_breach_cut_depth_m = 3.0;
    std::uint32_t maximum_breach_path_cells = 8;
    double maximum_breach_excavation_m3 = 100000.0;
    std::uint32_t maximum_breach_candidates = 2000;
};

// Used by standalone builds or tests to supply terrain without Map HTTP.
// The callback runs on a worker thread and must be safe to invoke there.
using TerrainReader = std::function<ElevationGrid(
    const ElevationGrid &, const QString &, const std::atomic<bool> &)>;

struct ServiceConfig
{
    SourceConfig source;
    QString output_directory;
    QString dataset = QStringLiteral("copernicus-glo30");
    std::uint64_t maximum_cells = 10000000;
    // Bounded number of simultaneous terrain-analysis jobs (1..4). Each
    // REST job can also issue source.max_concurrent_requests Map requests.
    // Keep the default at one for embedded integrations and large rasters.
    int max_concurrent_analyses = 1;
    TerrainReader terrain_reader; // optional; defaults to Map HTTP/in-process adapter
};

// Transport-independent Hydrology job service. No Qt HttpServer dependency.
// One instance owns its queue and persistent job metadata. The terrain_reader
// and any externally owned Map instance must outlive this service.
class AnalysisService final
{
public:
    explicit AnalysisService(const ServiceConfig &config);
    ~AnalysisService();
    AnalysisService(const AnalysisService &) = delete;
    AnalysisService &operator=(const AnalysisService &) = delete;

    bool initialize(QString *error);
    QString submit(const AnalysisRequest &request, QString *error);
    QJsonObject job(const QString &id) const;
    QJsonArray jobs() const;
    bool cancel(const QString &id);
    QByteArray rasterWindow(const QString &id, const QString &layer,
                            int x, int y, int width, int height, QString *error) const;
    QByteArray catchment(const QString &id, int x, int y, QString *error) const;

    // Transport-independent spatial queries on a completed persistent graph.
    // Trace reads only cells along its path; network extraction is bounded by
    // the selected-channel and segment budgets to limit memory and output.
    QJsonObject downstreamTrace(const QString &id, int x, int y,
                                double maximum_distance_km, QString *error) const;
    QJsonObject channelNetwork(const QString &id, double minimum_area_km2,
                               QString *error) const;
    // Polygonal D8 watershed for one outlet: GeoJSON MultiPolygon including
    // holes/disjoint components and domain-clipping diagnostics.
    QJsonObject channelNetworkWindow(const QString &id, double minimum_area_km2,
                                     const DrainageWindow &window, QString *error) const;
    QJsonObject watershedBoundary(const QString &id, int x, int y,
                                  QString *error) const;
    // A WGS84 GeoJSON Polygon/MultiPolygon rainfall *selection*. Returns
    // the topologically downstream-affected channels, not rainfall volumes.
    QJsonObject affectedNetwork(const QString &id, const QJsonObject &request,
                                QString *error) const;

    // UUID-addressed depression hierarchy for a completed terrain analysis.
    // Numeric basin positions are strictly internal and never public entity IDs.
    QJsonObject basins(const QString &id, int offset, int limit, QString *error) const;
    QJsonObject basin(const QString &id, const QString &basin_uuid, QString *error) const;
    QJsonObject basinStorage(const QString &id, const QString &basin_uuid,
                             double level_m, QString *error) const;
    QJsonObject basinStorageCurve(const QString &id, const QString &basin_uuid,
                                  int samples, QString *error) const;

    // Rainfall scenarios reuse one immutable terrain-analysis job. The
    // resulting row-major runoff raster is persisted separately by UUID;
    // no D8 routing, infiltration timing, or basin filling occurs here.
    QJsonObject createRainfallScenario(const QString &id, const QJsonObject &request,
                                       QString *error) const;
    QJsonArray rainfallScenarios(const QString &id, QString *error) const;
    QJsonObject rainfallScenario(const QString &id, const QString &scenario_uuid,
                                 QString *error) const;
    QByteArray rainfallRunoffWindow(const QString &id, const QString &scenario_uuid,
                                    int x, int y, int width, int height,
                                    QString *error) const;
    // Static redistribution of a saved scenario. The first request calculates
    // and atomically caches an immutable result; later requests read only the
    // requested records, with integrity validation and cache repair. Basin
    // totals are nested; sum only root basins for total storage.
    QJsonObject rainfallRedistribution(const QString &id, const QString &scenario_uuid,
                                       int offset, int limit, QString *error) const;

    // Schedule a bounded background redistribution. Repeated submissions for
    // one scenario return its active task, or the completed cached result.
    // Task IDs are UUIDs; task states are transient, but result caches survive
    // restarts. No HTTP or GUI dependency.
    QJsonObject startRainfallRedistribution(const QString &id, const QString &scenario_uuid,
                                           QString *error);
    QJsonObject rainfallRedistributionStatus(const QString &id,
                                              const QString &scenario_uuid,
                                              QString *error) const;
    bool cancelRainfallRedistribution(const QString &id, const QString &scenario_uuid,
                                      QString *error);
    // Cache-only result lookup: never runs a synchronous calculation.
    QJsonObject rainfallRedistributionReady(const QString &id, const QString &scenario_uuid,
                                            int offset, int limit, QString *error) const;

    // Static standing-water reconstruction is a separate derived, asynchronous
    // artifact. GET methods never recalculate a missing or invalid cache.
    QJsonObject startStaticInundation(const QString &id, const QString &scenario_uuid,
                                      QString *error);
    QJsonObject staticInundationStatus(const QString &id, const QString &scenario_uuid,
                                       QString *error) const;
    QByteArray staticInundationDepthWindow(const QString &id, const QString &scenario_uuid,
                                           int x, int y, int width, int height,
                                           QString *error) const;
    bool cancelStaticInundation(const QString &id, const QString &scenario_uuid,
                                QString *error);

    // Independent, opt-in dynamic reservoir simulations with EXPLICIT hydraulic
    // curves, controls and timed inflows. Results never modify scenario caches.
    QJsonObject startCoupledSimulation(const QString &id, const QJsonObject &request,
                                       QString *error);
    QJsonObject coupledSimulationStatus(const QString &id, const QString &simulation_id,
                                        QString *error) const;
    QJsonObject coupledSimulationSnapshots(const QString &id, const QString &simulation_id,
                                           int offset, int limit, QString *error) const;
    bool cancelCoupledSimulation(const QString &id, const QString &simulation_id,
                                 QString *error);

    static QByteArray layerMime(const QString &layer);

private:
    struct Job;
    struct RedistributionTask;
    struct InundationTask;
    struct CoupledTask
    {
        QString id;
        QString analysis_id;
        QString state = QStringLiteral("queued");
        QString error;
        QJsonObject request;
        std::atomic<bool> cancelled {false};
    };
    void runCoupledTask(const QSharedPointer<CoupledTask> &task);
    void runInundationTask(const QSharedPointer<InundationTask> &task);
    void runRedistributionTask(const QSharedPointer<RedistributionTask> &task);
    QJsonObject rainfallRedistributionImpl(const QString &id, const QString &scenario_uuid,
                                           int offset, int limit, bool allow_calculation,
                                           const std::atomic<bool> *cancel_requested,
                                           QString *error) const;
    QSharedPointer<Job> findJob(const QString &id) const;
    QJsonObject jobJson(const QSharedPointer<Job> &job) const;
    void restoreJobs();
    void runJob(const QSharedPointer<Job> &job);
    bool writeManifest(const QSharedPointer<Job> &job) const;
    QString jobDirectory(const QString &id) const;

    ServiceConfig config;
    mutable QMutex jobs_mutex;
    mutable QMutex redistribution_cache_mutex;
    mutable QMutex redistribution_tasks_mutex;
    QHash<QString, QSharedPointer<RedistributionTask>> redistribution_tasks;
    QHash<QString, QSharedPointer<InundationTask>> inundation_tasks;
    QThreadPool redistribution_workers;
    mutable QMutex coupled_tasks_mutex;
    QHash<QString, QSharedPointer<CoupledTask>> coupled_tasks;
    QThreadPool coupled_workers;
    QHash<QString, QSharedPointer<Job>> job_map;
    QThreadPool workers;
};

} // namespace Aowis::Hydrology
#endif
