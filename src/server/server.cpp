#include "server.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUrlQuery>
#include <cmath>
#include <cstddef>

namespace Aowis::Hydrology
{
namespace
{
using Status = HttpResponse::StatusCode;

HttpResponse jsonResponse(const QJsonObject &json, Status status = Status::Ok)
{
    return HttpResponse(QByteArrayLiteral("application/json"),
                               QJsonDocument(json).toJson(QJsonDocument::Compact), status);
}

HttpResponse errorResponse(const QString &error, Status status)
{
    return jsonResponse(QJsonObject{{QStringLiteral("error"), error}}, status);
}

bool readInt(const QUrlQuery &query, const QString &name, int *value)
{
    bool ok = false;
    *value = query.queryItemValue(name).toInt(&ok);
    return ok;
}

bool readDouble(const QJsonObject &json, const QString &name, double *value)
{
    if (!json.value(name).isDouble()) return false;
    *value = json.value(name).toDouble();
    return std::isfinite(*value);
}

ServiceConfig serviceConfig(const ServerConfig &config)
{
    ServiceConfig result;
    result.source = config.source;
    result.output_directory = config.output_directory;
    result.dataset = config.dataset;
    result.maximum_cells = config.maximum_cells;
    result.maximum_coupled_dem_cells = config.maximum_coupled_dem_cells;
    result.max_concurrent_analyses = config.max_concurrent_analyses;
    result.max_outstanding_analyses = config.max_outstanding_analyses;
    return result;
}
} // namespace

Server::Server(const ServerConfig &config)
    : config(config), service(serviceConfig(config))
{
    // Independent HTTP workers must not own any Qt event-loop objects.
    // Numerical work is admitted through the service's bounded queues.
    constexpr std::size_t maxHttpBodyBytes = 256 * 1024;
    // cpp-httplib enforces this limit while reading a body, but a client
    // declaring an oversized Content-Length without sending the body would
    // otherwise tie up a worker until read_timeout. Reject it from headers,
    // before entering the route's body reader. Chunked/decoded bodies remain
    // guarded by the transport's payload limit below.
    this->http.set_payload_max_length(maxHttpBodyBytes);
    this->http.set_pre_routing_handler([](const httplib::Request &request,
                                          httplib::Response &response) {
        if (request.has_header("Content-Length") &&
            request.get_header_value_u64("Content-Length") > maxHttpBodyBytes)
        {
            response.status = httplib::StatusCode::PayloadTooLarge_413;
            response.set_content(R"({"error":"HTTP request body too large"})",
                                 "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    this->http.set_read_timeout(config.http_read_timeout_s);
    this->http.set_write_timeout(config.http_write_timeout_s);
    this->http.set_keep_alive_timeout(config.http_keep_alive_timeout_s);
    this->http.new_task_queue = [workers = config.http_workers,
                                 queued = config.http_max_queued_requests]() {
        return new httplib::ThreadPool(size_t(workers), size_t(workers), size_t(queued));
    };
    this->http.set_error_handler([](const httplib::Request &, httplib::Response &response)
        -> httplib::Server::HandlerResponse {
        // Preserve the precise domain error and its HTTP status. Only synthesize
        // a body for transport-level errors such as 404 or oversized requests.
        if (!response.body.empty())
            return httplib::Server::HandlerResponse::Unhandled;
        const QByteArray body = QJsonDocument(QJsonObject{
            {QStringLiteral("error"), response.status == 413
                ? QStringLiteral("HTTP request body too large")
                : QStringLiteral("HTTP request failed")}}).toJson(QJsonDocument::Compact);
        response.set_content(body.toStdString(), "application/json");
        return httplib::Server::HandlerResponse::Handled;
    });
    setupRoutes();
}

Server::~Server()
{
    this->http.stop();
    if (this->listener.joinable()) this->listener.join();
    // The service is destroyed after all HTTP workers have stopped.
}

bool Server::start(QString *error_message)
{
    if (!this->service.initialize(error_message)) return false;
    const std::string host = this->config.listen_address.toString().toStdString();
    if (!this->http.bind_to_port(host, this->config.port))
    {
        if (error_message) *error_message = QStringLiteral("Cannot bind Hydrology HTTP listener to %1:%2")
            .arg(this->config.listen_address.toString()).arg(this->config.port);
        return false;
    }
    this->listener = std::thread([this] { this->http.listen_after_bind(); });
    return true;
}

void Server::setupRoutes()
{
    this->http.route("/status", HttpRequest::Method::Get, [this]()
    {
        return jsonResponse(QJsonObject{
            {QStringLiteral("service"), QStringLiteral("aowis-server-hydrology")},
            {QStringLiteral("status"), QStringLiteral("ok")},
            {QStringLiteral("max_concurrent_analyses"), this->config.max_concurrent_analyses},
            {QStringLiteral("max_cells"), double(this->config.maximum_cells)},
            {QStringLiteral("max_coupled_dem_cells"), double(this->config.maximum_coupled_dem_cells)},
            {QStringLiteral("max_outstanding_analyses"), this->config.max_outstanding_analyses},
            {QStringLiteral("http_workers"), this->config.http_workers},
            {QStringLiteral("http_max_queued_requests"), this->config.http_max_queued_requests},
            {QStringLiteral("terrain_jobs"), this->service.terrainJobCapacity()},
            {QStringLiteral("map_requests_per_analysis"), this->config.source.max_concurrent_requests}});
    });

    this->http.route("/v1/jobs", HttpRequest::Method::Post,
                     [this](const HttpRequest &request)
    {
        if (request.body().size() > 16384)
            return errorResponse(QStringLiteral("Job request too large"), Status::BadRequest);
        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(request.body(), &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject())
            return errorResponse(QStringLiteral("Expected JSON request object"), Status::BadRequest);
        const QJsonObject json = document.object();
        AnalysisRequest analysis;
        if (!readDouble(json, QStringLiteral("west"), &analysis.west) ||
            !readDouble(json, QStringLiteral("south"), &analysis.south) ||
            !readDouble(json, QStringLiteral("east"), &analysis.east) ||
            !readDouble(json, QStringLiteral("north"), &analysis.north))
            return errorResponse(QStringLiteral("Provide numeric WGS84 bounds"), Status::BadRequest);
        if (json.contains(QStringLiteral("resolution_m")) &&
            !readDouble(json, QStringLiteral("resolution_m"), &analysis.resolution_m))
            return errorResponse(QStringLiteral("Invalid resolution_m"), Status::BadRequest);
        analysis.dataset = json.value(QStringLiteral("dataset")).toString();
        analysis.fill_depressions = json.value(QStringLiteral("fill_depressions")).toBool(true);
        analysis.breach_depressions = json.value(QStringLiteral("breach_depressions")).toBool(false);
        if (json.contains(QStringLiteral("maximum_breach_cut_depth_m")) &&
            !readDouble(json, QStringLiteral("maximum_breach_cut_depth_m"),
                        &analysis.maximum_breach_cut_depth_m))
            return errorResponse(QStringLiteral("Invalid maximum_breach_cut_depth_m"), Status::BadRequest);
        if (json.contains(QStringLiteral("maximum_breach_excavation_m3")) &&
            !readDouble(json, QStringLiteral("maximum_breach_excavation_m3"),
                        &analysis.maximum_breach_excavation_m3))
            return errorResponse(QStringLiteral("Invalid maximum_breach_excavation_m3"), Status::BadRequest);
        if (json.contains(QStringLiteral("maximum_breach_path_cells")))
        {
            const QJsonValue value = json.value(QStringLiteral("maximum_breach_path_cells"));
            if (!value.isDouble() || value.toDouble() != std::floor(value.toDouble()) ||
                value.toDouble() < 2 || value.toDouble() > 32)
                return errorResponse(QStringLiteral("Invalid maximum_breach_path_cells"), Status::BadRequest);
            analysis.maximum_breach_path_cells = std::uint32_t(value.toInt());
        }
        if (json.contains(QStringLiteral("maximum_breach_candidates")))
        {
            const QJsonValue value = json.value(QStringLiteral("maximum_breach_candidates"));
            if (!value.isDouble() || value.toDouble() != std::floor(value.toDouble()) ||
                value.toDouble() < 1 || value.toDouble() > 100000)
                return errorResponse(QStringLiteral("Invalid maximum_breach_candidates"), Status::BadRequest);
            analysis.maximum_breach_candidates = std::uint32_t(value.toInt());
        }
        QString error;
        const QString id = this->service.submit(analysis, &error);
        if (id.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Terrain analysis capacity"))
                ? static_cast<Status>(429) // Too Many Requests (all supported Qt 6 releases)
                : error.startsWith(QStringLiteral("Cannot"))
                ? Status::InternalServerError : Status::BadRequest);
        return jsonResponse(this->service.job(id), Status::Accepted);
    });

    this->http.route("/v1/jobs", HttpRequest::Method::Get, [this]()
    {
        return jsonResponse(QJsonObject{{QStringLiteral("jobs"), this->service.jobs()}});
    });

    this->http.route("/v1/jobs/<arg>", HttpRequest::Method::Get,
                     [this](const QString &id)
    {
        const QJsonObject found = this->service.job(id);
        if (found.isEmpty()) return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        return jsonResponse(found);
    });

    this->http.route("/v1/jobs/<arg>", HttpRequest::Method::Delete,
                     [this](const QString &id)
    {
        if (!this->service.cancel(id))
            return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        return jsonResponse(this->service.job(id), Status::Accepted);
    });

    // UUID-addressed basin hierarchy endpoints. The same operations are
    // available to an embedded GUI through AOWIS::HydrologyService.
    this->http.route("/v1/jobs/<arg>/basins", HttpRequest::Method::Get,
                     [this](const QString &id, const HttpRequest &request)
    {
        const QJsonObject job = this->service.job(id);
        if (job.isEmpty()) return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        if (job.value(QStringLiteral("state")) != QStringLiteral("completed"))
            return errorResponse(QStringLiteral("Analysis not completed"), Status::Conflict);
        const QUrlQuery query(request.url());
        int offset = 0;
        int limit = 50;
        if ((query.hasQueryItem(QStringLiteral("offset")) &&
             !readInt(query, QStringLiteral("offset"), &offset)) ||
            (query.hasQueryItem(QStringLiteral("limit")) &&
             !readInt(query, QStringLiteral("limit"), &limit)))
            return errorResponse(QStringLiteral("Invalid basin pagination"), Status::BadRequest);
        QString error;
        const QJsonObject result = this->service.basins(id, offset, limit, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid"))
                ? Status::BadRequest : Status::InternalServerError);
        return jsonResponse(result);
    });

    this->http.route("/v1/jobs/<arg>/basins/<arg>", HttpRequest::Method::Get,
                     [this](const QString &id, const QString &basin_uuid)
    {
        const QJsonObject job = this->service.job(id);
        if (job.isEmpty()) return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        if (job.value(QStringLiteral("state")) != QStringLiteral("completed"))
            return errorResponse(QStringLiteral("Analysis not completed"), Status::Conflict);
        QString error;
        const QJsonObject result = this->service.basin(id, basin_uuid, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Unknown basin"))
                ? Status::NotFound : error.startsWith(QStringLiteral("Invalid"))
                ? Status::BadRequest : Status::InternalServerError);
        return jsonResponse(result);
    });

    this->http.route("/v1/jobs/<arg>/basins/<arg>/storage", HttpRequest::Method::Get,
                     [this](const QString &id, const QString &basin_uuid,
                            const HttpRequest &request)
    {
        const QJsonObject job = this->service.job(id);
        if (job.isEmpty()) return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        if (job.value(QStringLiteral("state")) != QStringLiteral("completed"))
            return errorResponse(QStringLiteral("Analysis not completed"), Status::Conflict);
        const QUrlQuery query(request.url());
        bool valid = false;
        const double level = query.queryItemValue(QStringLiteral("level_m")).toDouble(&valid);
        if (!valid || !std::isfinite(level))
            return errorResponse(QStringLiteral("Invalid basin water level"), Status::BadRequest);
        QString error;
        const QJsonObject result = this->service.basinStorage(id, basin_uuid, level, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid")) ||
                error.startsWith(QStringLiteral("Water level"))
                ? Status::BadRequest : error.startsWith(QStringLiteral("Unknown basin"))
                ? Status::NotFound : Status::InternalServerError);
        return jsonResponse(result);
    });

    this->http.route("/v1/jobs/<arg>/basins/<arg>/curve", HttpRequest::Method::Get,
                     [this](const QString &id, const QString &basin_uuid,
                            const HttpRequest &request)
    {
        const QJsonObject job = this->service.job(id);
        if (job.isEmpty()) return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        if (job.value(QStringLiteral("state")) != QStringLiteral("completed"))
            return errorResponse(QStringLiteral("Analysis not completed"), Status::Conflict);
        const QUrlQuery query(request.url());
        int samples = 9;
        if (query.hasQueryItem(QStringLiteral("samples")) &&
            !readInt(query, QStringLiteral("samples"), &samples))
            return errorResponse(QStringLiteral("Invalid curve sample count"), Status::BadRequest);
        QString error;
        const QJsonObject result = this->service.basinStorageCurve(id, basin_uuid, samples, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid"))
                ? Status::BadRequest : error.startsWith(QStringLiteral("Unknown basin"))
                ? Status::NotFound : Status::InternalServerError);
        return jsonResponse(result);
    });

    this->http.route("/v1/jobs/<arg>/raster/<arg>", HttpRequest::Method::Get,
                     [this](const QString &id, const QString &layer,
                            const HttpRequest &request)
    {
        const QJsonObject job = this->service.job(id);
        if (job.isEmpty()) return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        if (job.value(QStringLiteral("state")) != QStringLiteral("completed"))
            return errorResponse(QStringLiteral("Analysis not completed"), Status::Conflict);
        const QByteArray mime = AnalysisService::layerMime(layer);
        if (mime.isEmpty())
            return errorResponse(QStringLiteral("Unknown raster layer"), Status::BadRequest);
        const QUrlQuery query(request.url());
        int x = 0, y = 0, width = 0, height = 0;
        if (!readInt(query, QStringLiteral("x"), &x) ||
            !readInt(query, QStringLiteral("y"), &y) ||
            !readInt(query, QStringLiteral("width"), &width) ||
            !readInt(query, QStringLiteral("height"), &height))
            return errorResponse(QStringLiteral("Invalid raster window"), Status::BadRequest);
        QString error;
        const QByteArray data = this->service.rasterWindow(id, layer, x, y, width, height, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid"))
                ? Status::BadRequest : Status::InternalServerError);
        return HttpResponse(mime, data);
    });

    this->http.route("/v1/jobs/<arg>/trace", HttpRequest::Method::Get,
                     [this](const QString &id, const HttpRequest &request)
    {
        const QJsonObject job = this->service.job(id);
        if (job.isEmpty()) return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        if (job.value(QStringLiteral("state")) != QStringLiteral("completed"))
            return errorResponse(QStringLiteral("Analysis not completed"), Status::Conflict);
        const QUrlQuery query(request.url());
        int x = 0, y = 0;
        if (!readInt(query, QStringLiteral("x"), &x) ||
            !readInt(query, QStringLiteral("y"), &y))
            return errorResponse(QStringLiteral("Invalid trace origin"), Status::BadRequest);
        bool distance_ok = true;
        double distance_km = 20.0;
        if (query.hasQueryItem(QStringLiteral("distance_km")))
        {
            distance_km = query.queryItemValue(QStringLiteral("distance_km")).toDouble(&distance_ok);
            if (!distance_ok || !std::isfinite(distance_km))
                return errorResponse(QStringLiteral("Invalid trace distance"), Status::BadRequest);
        }
        QString error;
        const QJsonObject feature = this->service.downstreamTrace(id, x, y, distance_km, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid"))
                ? Status::BadRequest : Status::InternalServerError);
        return jsonResponse(feature);
    });

    this->http.route("/v1/jobs/<arg>/network", HttpRequest::Method::Get,
                     [this](const QString &id, const HttpRequest &request)
    {
        const QJsonObject job = this->service.job(id);
        if (job.isEmpty()) return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        if (job.value(QStringLiteral("state")) != QStringLiteral("completed"))
            return errorResponse(QStringLiteral("Analysis not completed"), Status::Conflict);
        const QUrlQuery query(request.url());
        bool valid = false;
        const double area_km2 = query.queryItemValue(
            QStringLiteral("min_upstream_area_km2")).toDouble(&valid);
        if (!valid || !std::isfinite(area_km2))
            return errorResponse(QStringLiteral("Invalid minimum contributing area"), Status::BadRequest);
        QString error;
        QJsonObject feature_collection;
        if (query.hasQueryItem(QStringLiteral("west")) ||
            query.hasQueryItem(QStringLiteral("east")) ||
            query.hasQueryItem(QStringLiteral("south")) ||
            query.hasQueryItem(QStringLiteral("north")))
        {
            bool west_ok = false, east_ok = false, south_ok = false, north_ok = false;
            DrainageWindow window;
            window.west = query.queryItemValue(QStringLiteral("west")).toDouble(&west_ok);
            window.east = query.queryItemValue(QStringLiteral("east")).toDouble(&east_ok);
            window.south = query.queryItemValue(QStringLiteral("south")).toDouble(&south_ok);
            window.north = query.queryItemValue(QStringLiteral("north")).toDouble(&north_ok);
            if (!west_ok || !east_ok || !south_ok || !north_ok)
                return errorResponse(QStringLiteral("Invalid drainage window coordinates"),
                                     Status::BadRequest);
            feature_collection = this->service.channelNetworkWindow(id, area_km2, window, &error);
        }
        else feature_collection = this->service.channelNetwork(id, area_km2, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid")) ||
                error.startsWith(QStringLiteral("Too many"))
                ? Status::BadRequest : Status::InternalServerError);
        return jsonResponse(feature_collection);
    });

    this->http.route("/v1/jobs/<arg>/watershed", HttpRequest::Method::Get,
                     [this](const QString &id, const HttpRequest &request)
    {
        const QJsonObject job = this->service.job(id);
        if (job.isEmpty()) return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        if (job.value(QStringLiteral("state")) != QStringLiteral("completed"))
            return errorResponse(QStringLiteral("Analysis not completed"), Status::Conflict);
        const QUrlQuery query(request.url());
        int x = 0, y = 0;
        if (!readInt(query, QStringLiteral("x"), &x) ||
            !readInt(query, QStringLiteral("y"), &y))
            return errorResponse(QStringLiteral("Invalid watershed outlet"), Status::BadRequest);
        QString error;
        const QJsonObject result = this->service.watershedBoundary(id, x, y, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Catchment calculation busy"))
                ? static_cast<Status>(429) : error.startsWith(QStringLiteral("Invalid")) ||
                error.startsWith(QStringLiteral("Too many"))
                ? Status::BadRequest : Status::InternalServerError);
        return jsonResponse(result);
    });

    this->http.route("/v1/jobs/<arg>/affected-network", HttpRequest::Method::Post,
                     [this](const QString &id, const HttpRequest &request)
    {
        const QJsonObject job = this->service.job(id);
        if (job.isEmpty()) return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        if (job.value(QStringLiteral("state")) != QStringLiteral("completed"))
            return errorResponse(QStringLiteral("Analysis not completed"), Status::Conflict);
        if (request.body().size() > 262144)
            return errorResponse(QStringLiteral("Polygon query exceeds 256 KiB"), Status::BadRequest);
        QJsonParseError parser_error;
        const QJsonDocument document = QJsonDocument::fromJson(request.body(), &parser_error);
        if (parser_error.error != QJsonParseError::NoError || !document.isObject())
            return errorResponse(QStringLiteral("Invalid polygon request JSON"), Status::BadRequest);
        QString error;
        const QJsonObject result = this->service.affectedNetwork(id, document.object(), &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid")) ||
                error.startsWith(QStringLiteral("Too many"))
                ? Status::BadRequest : Status::InternalServerError);
        return jsonResponse(result);
    });

    // Terrain analysis jobs may host many independent rainfall scenarios.
    // Scenario rasters and manifests are immutable after creation, so
    // fetching a scenario never reruns or changes the DEM analysis.
    this->http.route("/v1/jobs/<arg>/rainfall-scenarios", HttpRequest::Method::Post,
                     [this](const QString &id, const HttpRequest &request)
    {
        if (request.body().size() > 262144)
            return errorResponse(QStringLiteral("Rainfall request exceeds 256 KiB"), Status::BadRequest);
        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(request.body(), &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject())
            return errorResponse(QStringLiteral("Invalid rainfall scenario JSON"), Status::BadRequest);
        const QJsonObject job = this->service.job(id);
        if (job.isEmpty()) return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        if (job.value(QStringLiteral("state")) != QStringLiteral("completed"))
            return errorResponse(QStringLiteral("Analysis not completed"), Status::Conflict);
        QString error;
        const QJsonObject result = this->service.createRainfallScenario(id, document.object(), &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid")) ||
                error.startsWith(QStringLiteral("Too many")) ||
                error.startsWith(QStringLiteral("Antimeridian")) ||
                error.startsWith(QStringLiteral("Rainfall polygons"))
                ? Status::BadRequest : Status::InternalServerError);
        return jsonResponse(result, Status::Created);
    });

    // Optional asynchronous scenario creation. The legacy POST above keeps
    // its response contract; the task API does not block the HTTP event loop.
    this->http.route("/v1/jobs/<arg>/rainfall-scenario-tasks",
                     HttpRequest::Method::Post,
                     [this](const QString &id, const HttpRequest &request)
    {
        if (request.body().size() > 262144)
            return errorResponse(QStringLiteral("Rainfall request exceeds 256 KiB"), Status::BadRequest);
        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(request.body(), &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject())
            return errorResponse(QStringLiteral("Invalid rainfall scenario JSON"), Status::BadRequest);
        QString error;
        const QJsonObject result = this->service.startRainfallScenario(id, document.object(), &error);
        if (!error.isEmpty())
            return errorResponse(error,
                error.startsWith(QStringLiteral("Rainfall scenario queue full"))
                    ? static_cast<Status>(429) :
                error.startsWith(QStringLiteral("Unknown"))
                    ? Status::NotFound : Status::BadRequest);
        return jsonResponse(result, Status::Accepted);
    });

    this->http.route("/v1/jobs/<arg>/rainfall-scenario-tasks/<arg>",
                     HttpRequest::Method::Get,
                     [this](const QString &id, const QString &task_id)
    {
        QString error;
        const QJsonObject result = this->service.rainfallScenarioTask(id, task_id, &error);
        if (!error.isEmpty()) return errorResponse(error, Status::NotFound);
        return jsonResponse(result);
    });

    this->http.route("/v1/jobs/<arg>/rainfall-scenario-tasks/<arg>",
                     HttpRequest::Method::Delete,
                     [this](const QString &id, const QString &task_id)
    {
        QString error;
        if (!this->service.cancelRainfallScenarioTask(id, task_id, &error))
            return errorResponse(error, Status::NotFound);
        return jsonResponse(this->service.rainfallScenarioTask(id, task_id, &error),
                            Status::Accepted);
    });

    this->http.route("/v1/jobs/<arg>/rainfall-scenarios", HttpRequest::Method::Get,
                     [this](const QString &id)
    {
        if (this->service.job(id).isEmpty())
            return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        QString error;
        const QJsonArray items = this->service.rainfallScenarios(id, &error);
        if (!error.isEmpty())
            return errorResponse(error, Status::InternalServerError);
        return jsonResponse(QJsonObject{{QStringLiteral("scenarios"), items}});
    });

    this->http.route("/v1/jobs/<arg>/rainfall-scenarios/<arg>", HttpRequest::Method::Get,
                     [this](const QString &id, const QString &scenario_id)
    {
        if (this->service.job(id).isEmpty())
            return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        QString error;
        const QJsonObject result = this->service.rainfallScenario(id, scenario_id, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid")) ||
                error.startsWith(QStringLiteral("Unknown"))
                ? Status::NotFound : Status::InternalServerError);
        return jsonResponse(result);
    });

    this->http.route("/v1/jobs/<arg>/rainfall-scenarios/<arg>/runoff",
                     HttpRequest::Method::Get,
                     [this](const QString &id, const QString &scenario_id,
                            const HttpRequest &request)
    {
        if (this->service.job(id).isEmpty())
            return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        const QUrlQuery query(request.url());
        int x = 0, y = 0, width = 0, height = 0;
        if (!readInt(query, QStringLiteral("x"), &x) ||
            !readInt(query, QStringLiteral("y"), &y) ||
            !readInt(query, QStringLiteral("width"), &width) ||
            !readInt(query, QStringLiteral("height"), &height))
            return errorResponse(QStringLiteral("Invalid rainfall raster window"), Status::BadRequest);
        QString error;
        const QByteArray output = this->service.rainfallRunoffWindow(
            id, scenario_id, x, y, width, height, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid"))
                ? Status::BadRequest : error.startsWith(QStringLiteral("Unknown"))
                ? Status::NotFound : Status::InternalServerError);
        return HttpResponse(QByteArrayLiteral("application/vnd.aowis.raster-f64le"), output);
    });

    // Nonblocking background calculation, one active task per scenario.
    this->http.route("/v1/jobs/<arg>/rainfall-scenarios/<arg>/redistribution",
                     HttpRequest::Method::Post,
                     [this](const QString &id, const QString &scenario_id)
    {
        QString error;
        const QJsonObject result = this->service.startRainfallRedistribution(
            id, scenario_id, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid"))
                ? Status::BadRequest : error.startsWith(QStringLiteral("Unknown"))
                ? Status::NotFound : error.startsWith(QStringLiteral("Redistribution queue full"))
                ? Status::Conflict : Status::InternalServerError);
        return jsonResponse(result, result.value(QStringLiteral("cache_ready")).toBool()
            ? Status::Ok : Status::Accepted);
    });

    this->http.route("/v1/jobs/<arg>/rainfall-scenarios/<arg>/redistribution/status",
                     HttpRequest::Method::Get,
                     [this](const QString &id, const QString &scenario_id)
    {
        QString error;
        const QJsonObject result = this->service.rainfallRedistributionStatus(
            id, scenario_id, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid"))
                ? Status::BadRequest : error.startsWith(QStringLiteral("Unknown"))
                ? Status::NotFound : Status::InternalServerError);
        return jsonResponse(result);
    });

    this->http.route("/v1/jobs/<arg>/rainfall-scenarios/<arg>/redistribution",
                     HttpRequest::Method::Delete,
                     [this](const QString &id, const QString &scenario_id)
    {
        QString error;
        if (!this->service.cancelRainfallRedistribution(id, scenario_id, &error))
            return errorResponse(error, Status::Conflict);
        return jsonResponse(QJsonObject{{QStringLiteral("state"),
            QStringLiteral("cancel_requested")}}, Status::Accepted);
    });

    // Cache-only volume-balance result. GET never triggers computation; use
    // POST and poll the status route before retrieving paginated records.
    this->http.route("/v1/jobs/<arg>/rainfall-scenarios/<arg>/redistribution",
                     HttpRequest::Method::Get,
                     [this](const QString &id, const QString &scenario_id,
                            const HttpRequest &request)
    {
        const QJsonObject job = this->service.job(id);
        if (job.isEmpty()) return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        if (job.value(QStringLiteral("state")) != QStringLiteral("completed"))
            return errorResponse(QStringLiteral("Analysis not completed"), Status::Conflict);
        const QUrlQuery query(request.url());
        int offset = 0;
        int limit = 50;
        if ((query.hasQueryItem(QStringLiteral("offset")) &&
             !readInt(query, QStringLiteral("offset"), &offset)) ||
            (query.hasQueryItem(QStringLiteral("limit")) &&
             !readInt(query, QStringLiteral("limit"), &limit)))
            return errorResponse(QStringLiteral("Invalid redistribution pagination"), Status::BadRequest);
        QString error;
        const QJsonObject result = this->service.rainfallRedistributionReady(
            id, scenario_id, offset, limit, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid"))
                ? Status::BadRequest : error.startsWith(QStringLiteral("Unknown"))
                ? Status::NotFound : error.startsWith(QStringLiteral("Redistribution result not ready"))
                ? Status::Conflict : Status::InternalServerError);
        return jsonResponse(result);
    });


    // Inundation is a separately cached, conservative standing-water
    // reconstruction. POST queues work, GET status is nonblocking and
    // GET depth returns only fully published, checksum-checked raster windows.
    this->http.route("/v1/jobs/<arg>/rainfall-scenarios/<arg>/inundation",
                     HttpRequest::Method::Post,
                     [this](const QString &id, const QString &scenario_id)
    {
        QString error;
        const QJsonObject result = this->service.startStaticInundation(id, scenario_id, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid"))
                ? Status::BadRequest : error.startsWith(QStringLiteral("Unknown"))
                ? Status::NotFound : error.startsWith(QStringLiteral("Redistribution result not ready")) ||
                error.startsWith(QStringLiteral("Inundation queue full"))
                ? Status::Conflict : Status::InternalServerError);
        return jsonResponse(result, result.value(QStringLiteral("cache_ready")).toBool()
            ? Status::Ok : Status::Accepted);
    });

    this->http.route("/v1/jobs/<arg>/rainfall-scenarios/<arg>/inundation/status",
                     HttpRequest::Method::Get,
                     [this](const QString &id, const QString &scenario_id)
    {
        QString error;
        const QJsonObject result = this->service.staticInundationStatus(id, scenario_id, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid"))
                ? Status::BadRequest : error.startsWith(QStringLiteral("Unknown"))
                ? Status::NotFound : Status::InternalServerError);
        return jsonResponse(result);
    });

    this->http.route("/v1/jobs/<arg>/rainfall-scenarios/<arg>/inundation",
                     HttpRequest::Method::Delete,
                     [this](const QString &id, const QString &scenario_id)
    {
        QString error;
        if (!this->service.cancelStaticInundation(id, scenario_id, &error))
            return errorResponse(error, Status::Conflict);
        return jsonResponse(QJsonObject{{QStringLiteral("state"),
            QStringLiteral("cancel_requested")}}, Status::Accepted);
    });

    this->http.route("/v1/jobs/<arg>/rainfall-scenarios/<arg>/inundation/depth",
                     HttpRequest::Method::Get,
                     [this](const QString &id, const QString &scenario_id,
                            const HttpRequest &request)
    {
        const QUrlQuery query(request.url());
        int x = 0, y = 0, width = 0, height = 0;
        if (!readInt(query, QStringLiteral("x"), &x) ||
            !readInt(query, QStringLiteral("y"), &y) ||
            !readInt(query, QStringLiteral("width"), &width) ||
            !readInt(query, QStringLiteral("height"), &height))
            return errorResponse(QStringLiteral("Invalid inundation raster window"), Status::BadRequest);
        QString error;
        const QByteArray result = this->service.staticInundationDepthWindow(
            id, scenario_id, x, y, width, height, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid inundation raster window"))
                ? Status::BadRequest : error.startsWith(QStringLiteral("Unknown"))
                ? Status::NotFound : error.startsWith(QStringLiteral("Inundation unavailable")) ||
                  error.startsWith(QStringLiteral("Inundation result not ready")) ||
                  error.startsWith(QStringLiteral("Redistribution result not ready"))
                ? Status::Conflict : Status::InternalServerError);
        return HttpResponse(QByteArrayLiteral("application/vnd.aowis.raster-f32le"), result);
    });

    // Experimental time-dependent lumped-reservoir simulation. Inputs include
    // explicit stage-storage and calibrated controls. POST queues work; GET
    // reads only fully persisted, bounded time-series windows.
    this->http.route("/v1/jobs/<arg>/coupled-simulations", HttpRequest::Method::Post,
                     [this](const QString &id, const HttpRequest &request)
    {
        if (request.body().size() > 256 * 1024)
            return errorResponse(QStringLiteral("Invalid coupled simulation request size"),
                                 Status::BadRequest);
        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(request.body(), &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject())
            return errorResponse(QStringLiteral("Invalid coupled simulation JSON"), Status::BadRequest);
        QString error;
        const QJsonObject result = this->service.startCoupledSimulation(id, document.object(), &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid")) ||
                error.startsWith(QStringLiteral("Duplicate")) ||
                error.startsWith(QStringLiteral("Coupled DEM cell limit exceeded"))
                ? Status::BadRequest : error.startsWith(QStringLiteral("Unknown"))
                ? Status::NotFound : error.startsWith(QStringLiteral("Coupled simulation queue full"))
                ? Status::Conflict : Status::InternalServerError);
        return jsonResponse(result, Status::Accepted);
    });

    this->http.route("/v1/jobs/<arg>/coupled-simulations/<arg>",
                     HttpRequest::Method::Get,
                     [this](const QString &id, const QString &simulation_id)
    {
        QString error;
        const QJsonObject result = this->service.coupledSimulationStatus(id, simulation_id, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Unknown"))
                ? Status::NotFound : Status::InternalServerError);
        return jsonResponse(result);
    });

    this->http.route("/v1/jobs/<arg>/coupled-simulations/<arg>/snapshots",
                     HttpRequest::Method::Get,
                     [this](const QString &id, const QString &simulation_id,
                            const HttpRequest &request)
    {
        const QUrlQuery query(request.url());
        int offset = 0;
        int limit = 25;
        if ((query.hasQueryItem(QStringLiteral("offset")) &&
             !readInt(query, QStringLiteral("offset"), &offset)) ||
            (query.hasQueryItem(QStringLiteral("limit")) &&
             !readInt(query, QStringLiteral("limit"), &limit)))
            return errorResponse(QStringLiteral("Invalid coupled simulation pagination"), Status::BadRequest);
        QString error;
        const QJsonObject result = this->service.coupledSimulationSnapshots(
            id, simulation_id, offset, limit, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid"))
                ? Status::BadRequest : error.startsWith(QStringLiteral("Unknown"))
                ? Status::NotFound : error.startsWith(QStringLiteral("Coupled simulation result not ready"))
                ? Status::Conflict : Status::InternalServerError);
        return jsonResponse(result);
    });

    this->http.route("/v1/jobs/<arg>/coupled-simulations/<arg>",
                     HttpRequest::Method::Delete,
                     [this](const QString &id, const QString &simulation_id)
    {
        QString error;
        if (!this->service.cancelCoupledSimulation(id, simulation_id, &error))
            return errorResponse(error, Status::Conflict);
        return jsonResponse(QJsonObject{{QStringLiteral("state"),
            QStringLiteral("cancel_requested")}}, Status::Accepted);
    });

    // Prefer these asynchronous endpoints for large rasters. Work executes in
    // a bounded service-owned pool; task masks are transient, disk-backed,
    // and entirely independent of the current HTTP adapter.
    this->http.route("/v1/jobs/<arg>/catchment-tasks",
                     HttpRequest::Method::Post,
                     [this](const QString &id, const HttpRequest &request)
    {
        if (request.body().size() > 1024)
            return errorResponse(QStringLiteral("Catchment request too large"), Status::BadRequest);
        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(request.body(), &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject())
            return errorResponse(QStringLiteral("Invalid catchment JSON"), Status::BadRequest);
        const QJsonObject object = document.object();
        if (!object.value(QStringLiteral("x")).isDouble() ||
            !object.value(QStringLiteral("y")).isDouble() ||
            object.value(QStringLiteral("x")).toDouble() !=
                double(object.value(QStringLiteral("x")).toInt(-1)) ||
            object.value(QStringLiteral("y")).toDouble() !=
                double(object.value(QStringLiteral("y")).toInt(-1)))
            return errorResponse(QStringLiteral("Invalid catchment outlet coordinates"), Status::BadRequest);
        QString error;
        const QJsonObject result = this->service.startCatchment(
            id, object.value(QStringLiteral("x")).toInt(),
            object.value(QStringLiteral("y")).toInt(), &error);
        if (!error.isEmpty())
            return errorResponse(error,
                error.startsWith(QStringLiteral("Catchment task queue full"))
                    ? static_cast<Status>(429) :
                error.startsWith(QStringLiteral("Unknown")) ? Status::NotFound :
                error.startsWith(QStringLiteral("Analysis not completed")) ? Status::Conflict :
                Status::BadRequest);
        return jsonResponse(result, Status::Accepted);
    });

    this->http.route("/v1/jobs/<arg>/catchment-tasks/<arg>",
                     HttpRequest::Method::Get,
                     [this](const QString &id, const QString &task_id)
    {
        QString error;
        const QJsonObject result = this->service.catchmentTask(id, task_id, &error);
        if (!error.isEmpty()) return errorResponse(error, Status::NotFound);
        return jsonResponse(result);
    });

    this->http.route("/v1/jobs/<arg>/catchment-tasks/<arg>/mask",
                     HttpRequest::Method::Get,
                     [this](const QString &id, const QString &task_id)
    {
        QString error;
        const QByteArray mask = this->service.catchmentTaskMask(id, task_id, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Unknown"))
                ? Status::NotFound : error.startsWith(QStringLiteral("Catchment result not ready"))
                ? Status::Conflict : Status::InternalServerError);
        return HttpResponse(QByteArrayLiteral("application/vnd.aowis.catchment-u8"), mask);
    });

    this->http.route("/v1/jobs/<arg>/catchment-tasks/<arg>",
                     HttpRequest::Method::Delete,
                     [this](const QString &id, const QString &task_id)
    {
        QString error;
        if (!this->service.cancelCatchmentTask(id, task_id, &error))
            return errorResponse(error, Status::NotFound);
        return jsonResponse(this->service.catchmentTask(id, task_id, &error), Status::Accepted);
    });

    this->http.route("/v1/jobs/<arg>/catchment", HttpRequest::Method::Get,
                     [this](const QString &id, const HttpRequest &request)
    {
        const QJsonObject job = this->service.job(id);
        if (job.isEmpty()) return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        if (job.value(QStringLiteral("state")) != QStringLiteral("completed"))
            return errorResponse(QStringLiteral("Analysis not completed"), Status::Conflict);
        const QUrlQuery query(request.url());
        int x = 0, y = 0;
        if (!readInt(query, QStringLiteral("x"), &x) ||
            !readInt(query, QStringLiteral("y"), &y))
            return errorResponse(QStringLiteral("Invalid catchment outlet"), Status::BadRequest);
        QString error;
        const QByteArray data = this->service.catchment(id, x, y, &error);
        if (!error.isEmpty())
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid"))
                ? Status::BadRequest : error.startsWith(QStringLiteral("Catchment calculation busy"))
                ? static_cast<Status>(429) : Status::InternalServerError);
        return HttpResponse(QByteArrayLiteral("application/vnd.aowis.catchment-u8"), data);
    });
}
} // namespace Aowis::Hydrology
