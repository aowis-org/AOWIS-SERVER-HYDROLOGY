#ifndef AOWIS_HYDROLOGY_SERVER_H
#define AOWIS_HYDROLOGY_SERVER_H

#include <aowis/hydrology/service.h>

#include <QHostAddress>
#include <QHttpServer>
#include <QString>
#include <QTcpServer>

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
    int max_concurrent_analyses = 1;
};

// Thin HTTP adapter. All analysis/jobs/results are owned by AnalysisService.
class Server final : public QObject
{
public:
    explicit Server(const ServerConfig &config, QObject *parent = nullptr);
    bool start(QString *error_message);

private:
    void setupRoutes();

    ServerConfig config;
    AnalysisService service;
    QHttpServer http;
    QTcpServer tcp;
};

} // namespace Aowis::Hydrology
#endif
