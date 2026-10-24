#ifndef AOWIS_HYDROLOGY_SERVER_H
#define AOWIS_HYDROLOGY_SERVER_H

#include <aowis/hydrology/service.h>
#include "http_adapter.h"

#include <QHostAddress>
#include <QString>
#include <thread>

namespace Aowis::Hydrology
{
struct ServerConfig
{
    QHostAddress listen_address = QHostAddress::LocalHost;
    quint16 port = 8123;
    SourceConfig source;
    QString output_directory;
    QString dataset = QStringLiteral("copernicus-glo30");
    std::uint64_t maximum_cells = 10000000;
    // DEM-based coupled geometry/scenario forcing is an additional costly
    // workload. Keep its independently configurable ceiling explicit.
    std::uint64_t maximum_coupled_dem_cells = 10000000;
    int max_concurrent_analyses = 1;
    int max_outstanding_analyses = 8;
    std::uint64_t max_active_terrain_cells = 0;
    int http_workers = 8;
    int http_max_queued_requests = 32;
    int http_read_timeout_s = 15;
    int http_write_timeout_s = 30;
    int http_keep_alive_timeout_s = 5;
    int max_concurrent_heavy_queries = 2;
    std::uint64_t hierarchy_cache_basins = 4000000;
    // When non-empty, every route except GET /status requires this key in an
    // "X-API-Key" header or as "Authorization: Bearer <key>".
    QByteArray api_key;
};

// Transport adapter; application and numerical state belong to AnalysisService.
class Server final
{
public:
    explicit Server(const ServerConfig &config);
    ~Server();
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;
    bool start(QString *error_message);

private:
    void setupRoutes();

    ServerConfig config;
    AnalysisService service;
    HttpAdapter http;
    std::thread listener;
};

} // namespace Aowis::Hydrology
#endif
