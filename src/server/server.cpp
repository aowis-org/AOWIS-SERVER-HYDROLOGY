#include "server.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QHttpServerResponse>
#include <QUrlQuery>
#include <cmath>

namespace Aowis::Hydrology
{
namespace
{
using Status = QHttpServerResponse::StatusCode;

QHttpServerResponse jsonResponse(const QJsonObject &json, Status status = Status::Ok)
{
    return QHttpServerResponse(QByteArrayLiteral("application/json"),
                               QJsonDocument(json).toJson(QJsonDocument::Compact), status);
}

QHttpServerResponse errorResponse(const QString &error, Status status)
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
    return result;
}
} // namespace

Server::Server(const ServerConfig &config, QObject *parent)
    : QObject(parent), config(config), service(serviceConfig(config))
{
    setupRoutes();
}

bool Server::start(QString *error_message)
{
    if (!this->service.initialize(error_message))
        return false;
    if (!this->tcp.listen(this->config.listen_address, this->config.port) ||
        !this->http.bind(&this->tcp))
    {
        if (error_message != nullptr) *error_message = this->tcp.errorString();
        return false;
    }
    return true;
}

void Server::setupRoutes()
{
    this->http.route("/status", QHttpServerRequest::Method::Get, []()
    {
        return jsonResponse(QJsonObject{{QStringLiteral("service"),
            QStringLiteral("aowis-server-hydrology")},
            {QStringLiteral("status"), QStringLiteral("ok")}});
    });

    this->http.route("/v1/jobs", QHttpServerRequest::Method::Post,
                     [this](const QHttpServerRequest &request)
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
            return errorResponse(error, error.startsWith(QStringLiteral("Cannot"))
                ? Status::InternalServerError : Status::BadRequest);
        return jsonResponse(this->service.job(id), Status::Accepted);
    });

    this->http.route("/v1/jobs", QHttpServerRequest::Method::Get, [this]()
    {
        return jsonResponse(QJsonObject{{QStringLiteral("jobs"), this->service.jobs()}});
    });

    this->http.route("/v1/jobs/<arg>", QHttpServerRequest::Method::Get,
                     [this](const QString &id)
    {
        const QJsonObject found = this->service.job(id);
        if (found.isEmpty()) return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        return jsonResponse(found);
    });

    this->http.route("/v1/jobs/<arg>", QHttpServerRequest::Method::Delete,
                     [this](const QString &id)
    {
        if (!this->service.cancel(id))
            return errorResponse(QStringLiteral("Unknown job"), Status::NotFound);
        return jsonResponse(this->service.job(id), Status::Accepted);
    });

    this->http.route("/v1/jobs/<arg>/raster/<arg>", QHttpServerRequest::Method::Get,
                     [this](const QString &id, const QString &layer,
                            const QHttpServerRequest &request)
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
        return QHttpServerResponse(mime, data);
    });

    this->http.route("/v1/jobs/<arg>/trace", QHttpServerRequest::Method::Get,
                     [this](const QString &id, const QHttpServerRequest &request)
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

    this->http.route("/v1/jobs/<arg>/network", QHttpServerRequest::Method::Get,
                     [this](const QString &id, const QHttpServerRequest &request)
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

    this->http.route("/v1/jobs/<arg>/watershed", QHttpServerRequest::Method::Get,
                     [this](const QString &id, const QHttpServerRequest &request)
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
            return errorResponse(error, error.startsWith(QStringLiteral("Invalid")) ||
                error.startsWith(QStringLiteral("Too many"))
                ? Status::BadRequest : Status::InternalServerError);
        return jsonResponse(result);
    });

    this->http.route("/v1/jobs/<arg>/affected-network", QHttpServerRequest::Method::Post,
                     [this](const QString &id, const QHttpServerRequest &request)
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

    this->http.route("/v1/jobs/<arg>/catchment", QHttpServerRequest::Method::Get,
                     [this](const QString &id, const QHttpServerRequest &request)
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
                ? Status::BadRequest : Status::InternalServerError);
        return QHttpServerResponse(QByteArrayLiteral("application/vnd.aowis.catchment-u8"), data);
    });
}
} // namespace Aowis::Hydrology
