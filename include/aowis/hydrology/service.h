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
#include <QQueue>
#include <QSemaphore>
#include <QSharedPointer>
#include <QString>
#include <QThreadPool>
#include <QTemporaryDir>

#include <memory>

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

struct CachedBasinHierarchy; // service-internal decoded hierarchy + UUID index

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
    // DEM-based coupled geometry/scenario forcing is an additional costly
    // workload. Keep its independently configurable ceiling explicit.
    std::uint64_t maximum_coupled_dem_cells = 10000000;
    // Bounded number of simultaneous terrain-analysis jobs (1..4). Each
    // REST job can also issue source.max_concurrent_requests Map requests.
    // Keep the default at one for embedded integrations and large rasters.
    int max_concurrent_analyses = 1;
    // Maximum accepted terrain analyses that are queued or running together.
    // Independent from completed/failed/canceled results, which stay on disk.
    // Must be >= max_concurrent_analyses (1..64).
    int max_outstanding_analyses = 8;
    // Bound the *combined* cells in dispatched terrain workers, not merely
    // their number. Zero (default) inherits maximum_cells. This is a workload
    // proxy, NOT a guaranteed peak-RSS bound; rainfall/coupled tasks have
    // separate queues and can overlap terrain computations.
    std::uint64_t max_active_terrain_cells = 0;
    // Synchronous whole-grid queries (channel networks, affected networks,
    // basin storage/curves, uncached hierarchy loads, synchronous rainfall
    // scenarios) each materialize full-grid arrays of up to 16 bytes/cell on
    // the calling thread. At most this many run at once (1..16); further
    // callers fail fast with "Heavy query capacity busy" instead of
    // multiplying peak memory by the number of HTTP workers.
    int max_concurrent_heavy_queries = 2;
    // Decoded basin hierarchies (with a UUID index) kept in memory for
    // basin listing/lookup, bounded by the total number of cached basins.
    std::uint64_t hierarchy_cache_basins = 4000000;
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

    // Removes catchment spools left behind by killed processes. SHA-256
    // verification of restored analyses runs in the background; such a job
    // reports "artifact_verification": "pending" and its data endpoints
    // fail with "Analysis integrity verification in progress" until done.
    bool initialize(QString *error);
    // Blocks until the background verification of restored analyses has
    // finished (embedders wanting the former blocking startup call this right
    // after initialize()). Returns false on timeout; -1 waits indefinitely.
    bool waitForArtifactVerification(int timeout_ms = -1);
    QString submit(const AnalysisRequest &request, QString *error);
    QJsonObject job(const QString &id) const;
    QJsonArray jobs() const;
    // Admission and queue state for status reporting, including in-flight
    // submissions that have reserved capacity but not yet published a job.
    QJsonObject terrainJobCapacity() const;
    bool cancel(const QString &id);
    QByteArray rasterWindow(const QString &id, const QString &layer,
                            int x, int y, int width, int height, QString *error) const;
    QByteArray catchment(const QString &id, int x, int y, QString *error) const;
    // Heavy full-grid catchments can be requested without occupying an HTTP
    // handler. Task state/masks are transient; masks are spooled to disk and
    // the bounded task cache is discarded when this service stops.
    QJsonObject startCatchment(const QString &id, int x, int y, QString *error);
    QJsonObject catchmentTask(const QString &id, const QString &task_id,
                              QString *error) const;
    QByteArray catchmentTaskMask(const QString &id, const QString &task_id,
                                 QString *error) const;
    bool cancelCatchmentTask(const QString &id, const QString &task_id,
                             QString *error);

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
    // Non-blocking scenario creation. Unlike the legacy synchronous method,
    // task submission never loads a DEM or calculates polygon coverage.
    // A bounded cancelable queue is owned by the service (HTTP independent).
    QJsonObject startRainfallScenario(const QString &id, const QJsonObject &request,
                                     QString *error);
    QJsonObject rainfallScenarioTask(const QString &id, const QString &task_id,
                                    QString *error) const;
    bool cancelRainfallScenarioTask(const QString &id, const QString &task_id,
                                   QString *error);
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
    struct RainfallTask;
    struct CatchmentTask
    {
        QString id;
        QString analysis_id;
        int x = 0;
        int y = 0;
        QString state = QStringLiteral("queued");
        QString error;
        QString mask_path;
        std::atomic<bool> cancelled {false};
    };
    struct RedistributionTask;
    struct InundationTask;
    struct CoupledTask
    {
        QString id;
        QString analysis_id;
        QString state = QStringLiteral("queued");
        QString error;
        QJsonObject request; // released once the worker finishes; on disk in the manifest
        std::atomic<bool> cancelled {false};
    };
    QJsonObject createRainfallScenarioImpl(const QString &id, const QJsonObject &request,
                                           const std::atomic<bool> *cancel_requested,
                                           QString *error) const;
    QByteArray catchmentImpl(const QString &id, int x, int y,
                             const std::atomic<bool> *cancel_requested,
                             QString *error) const;
    void scheduleCatchmentTasks();
    void runCatchmentTask(const QSharedPointer<CatchmentTask> &task);
    QJsonObject catchmentTaskJson(const QSharedPointer<CatchmentTask> &task) const;
    void pruneCatchmentResultsLocked();
    void scheduleRainfallTasks();
    void runRainfallTask(const QSharedPointer<RainfallTask> &task);
    QJsonObject rainfallTaskJson(const QSharedPointer<RainfallTask> &task) const;
    void runCoupledTask(const QSharedPointer<CoupledTask> &task);
    void runInundationTask(const QSharedPointer<InundationTask> &task);
    void runRedistributionTask(const QSharedPointer<RedistributionTask> &task);
    QJsonObject rainfallRedistributionImpl(const QString &id, const QString &scenario_uuid,
                                           int offset, int limit, bool allow_calculation,
                                           const std::atomic<bool> *cancel_requested,
                                           QString *error) const;
    QSharedPointer<Job> findJob(const QString &id) const;
    // Completed AND integrity-verified job, or null with an error message.
    QSharedPointer<Job> completedJob(const QString &id, QString *error) const;
    // Decoded hierarchy with UUID index from the bounded cache. A cache miss
    // takes a heavy-query slot while decoding. Throws on failure or when busy.
    std::shared_ptr<const CachedBasinHierarchy> cachedHierarchy(const QSharedPointer<Job> &job) const;
    void verifyRestoredArtifacts();
    void pruneCoupledTasksLocked();
    QJsonObject jobJson(const QSharedPointer<Job> &job) const;
    void restoreJobs();
    void runJob(const QSharedPointer<Job> &job);
    void scheduleTerrainJobs();
    bool writeManifest(const QSharedPointer<Job> &job,
                       const QString &state_override = {}) const;
    QString jobDirectory(const QString &id) const;

    ServiceConfig config;
    mutable QMutex jobs_mutex;
    // Reserve scenario slots before CPU work, including synchronous callers.
    mutable QMutex rainfall_creation_mutex;
    mutable QHash<QString, int> rainfall_creation_reservations;
    // Counts of committed scenario manifests, initialized lazily from disk.
    // Protected by rainfall_creation_mutex alongside in-flight reservations.
    mutable QHash<QString, int> rainfall_committed_scenario_counts;
    mutable QMutex rainfall_tasks_mutex;
    QHash<QString, QSharedPointer<RainfallTask>> rainfall_tasks;
    QQueue<QString> rainfall_task_order; // bounded terminal-status retention
    QQueue<QSharedPointer<RainfallTask>> rainfall_queue;
    int rainfall_worker_slots = 0;
    QThreadPool rainfall_workers;
    // Shared by legacy synchronous calls and asynchronous worker tasks.
    // Prevent multiplying multi-gigabyte graph allocations when HTTP becomes
    // multithreaded. An ordinary sync caller is rejected rather than blocked.
    mutable QSemaphore catchment_calculation_slot {1};
    mutable QMutex catchment_tasks_mutex;
    QHash<QString, QSharedPointer<CatchmentTask>> catchment_tasks;
    QQueue<QString> catchment_task_order;
    QQueue<QSharedPointer<CatchmentTask>> catchment_queue;
    int catchment_worker_slots = 0;
    QThreadPool catchment_workers;
    std::unique_ptr<QTemporaryDir> catchment_spool;
    mutable QMutex redistribution_cache_mutex;
    mutable QMutex redistribution_tasks_mutex;
    QHash<QString, QSharedPointer<RedistributionTask>> redistribution_tasks;
    QHash<QString, QSharedPointer<InundationTask>> inundation_tasks;
    QThreadPool redistribution_workers;
    mutable QMutex coupled_tasks_mutex;
    QHash<QString, QSharedPointer<CoupledTask>> coupled_tasks;
    QQueue<QString> coupled_task_order; // bounded terminal-status retention
    QThreadPool coupled_workers;
    // Bounds concurrent synchronous whole-grid queries; see ServiceConfig.
    mutable QSemaphore heavy_query_slots {0}; // released to the configured count in initialize()
    mutable QMutex hierarchy_cache_mutex;
    mutable QHash<QString, std::shared_ptr<const CachedBasinHierarchy>> hierarchy_cache;
    mutable QQueue<QString> hierarchy_cache_order; // least recently used first
    mutable std::uint64_t hierarchy_cache_size = 0;
    bool initialized = false;
    std::atomic<bool> stopping {false};
    QThreadPool verification_workers;
    QHash<QString, QSharedPointer<Job>> job_map;
    QQueue<QSharedPointer<Job>> terrain_queue; // cancelable before worker dispatch
    int terrain_worker_slots = 0; // active runnable slots, guarded by jobs_mutex
    std::uint64_t active_terrain_cells = 0; // sum for dispatched workers, under jobs_mutex
    // Protected by jobs_mutex. Reserve a slot before touching the filesystem
    // to make admission atomic even with concurrent library API callers.
    int terrain_admission_reservations = 0;
    QThreadPool workers;
};

} // namespace Aowis::Hydrology
#endif
