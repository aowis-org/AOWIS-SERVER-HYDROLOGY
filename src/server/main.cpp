#include "server.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>
#include <cstdlib>
#include <thread>

#if defined(Q_OS_UNIX)
#include <csignal>
#include <pthread.h>
#endif


namespace
{
QString exampleConfig()
{
    return QStringLiteral(R"INI(; AOWIS Hydrology service. No file is created unless explicitly requested.
; Configure the AOWIS map server separately; this component never downloads DEMs.
[server]
listen_address=127.0.0.1
port=8123
output_directory=
max_cells=10000000
; DEM-dependent coupled simulations have a separate cap (4..100000000).
; Without DEM geometry or scenario forcing, explicit reservoir runs need no DEM.
max_coupled_dem_cells=10000000
; Independent analysis workers (1..4). Default 1 to limit memory use.
max_concurrent_analyses=1
; Maximum queued plus active terrain jobs (1..64, at least worker count).
max_outstanding_analyses=8
; Combined raster cells in running terrain jobs; 0 inherits max_cells.
; This is a concurrency guard, NOT a physical process-memory limit.
max_active_terrain_cells=0
; Bounded cpp-httplib request workers and queued connections.
http_workers=8
http_max_queued_requests=32
http_read_timeout_s=15
http_write_timeout_s=30
http_keep_alive_timeout_s=5
; Synchronous whole-grid queries (channel networks, basin storage, uncached
; basin hierarchies, synchronous rainfall scenarios) allowed at once (1..16).
; Each can hold up to 16 bytes per DEM cell; others get HTTP 429.
max_concurrent_heavy_queries=2
; Decoded basin hierarchies kept in memory, as a total basin count.
hierarchy_cache_basins=4000000
; Optional shared secret (16..256 printable ASCII characters). When set, all
; routes except GET /status require "X-API-Key: <key>" or
; "Authorization: Bearer <key>".
api_key=
; A non-loopback listen_address requires api_key unless this is true.
allow_unauthenticated_remote=false

[terrain]
dataset=copernicus-glo30
mode=rest
map_url=http://127.0.0.1:8122/
; Optional if Map already has a read API key configured:
map_read_api_key=
request_timeout_ms=180000
patch_size=256
max_concurrent_requests=4
; Standalone integrations supply their already-initialized Map library instance
; to AOWIS::HydrologyService; the executable always uses Map REST.
)INI");
}

#if defined(Q_OS_UNIX)
// SIGINT/SIGTERM are blocked in every thread (the mask is inherited by
// threads created later) and received synchronously by one watcher thread,
// which asks the Qt event loop to quit. main() then returns normally, so the
// server and the service shut down in order: HTTP stops, jobs are canceled
// and joined, and the transient catchment spool is removed. A second signal
// during a stuck shutdown terminates immediately.
void blockTerminationSignals(sigset_t *set)
{
    sigemptyset(set);
    sigaddset(set, SIGINT);
    sigaddset(set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, set, nullptr);
}

void startTerminationWatcher(const sigset_t &set)
{
    std::thread([set]()
    {
        int received = 0;
        if (sigwait(&set, &received) != 0) return;
        QMetaObject::invokeMethod(QCoreApplication::instance(), &QCoreApplication::quit,
                                  Qt::QueuedConnection);
        if (sigwait(&set, &received) == 0)
            std::_Exit(128 + received);
    }).detach();
}
#endif

bool loadConfig(const QString &file_path, bool explicitly_requested,
                Aowis::Hydrology::ServerConfig *config, QString *error)
{
    const QFileInfo file_info(file_path);
    if (!file_info.exists())
    {
        if (explicitly_requested)
        {
            *error = QStringLiteral("Configuration file does not exist: %1").arg(file_path);
            return false;
        }
        return true;
    }
    if (!file_info.isFile() || !file_info.isReadable())
    {
        *error = QStringLiteral("Configuration is not a readable file: %1").arg(file_path);
        return false;
    }
    const QDir relative_directory(file_info.absolutePath());
    QSettings settings(file_path, QSettings::IniFormat);
    if (settings.contains(QStringLiteral("server/listen_address")))
    {
        const QHostAddress address(settings.value(QStringLiteral("server/listen_address")).toString());
        if (address.isNull())
        {
            *error = QStringLiteral("Invalid server listen address");
            return false;
        }
        config->listen_address = address;
    }
    bool valid = true;
    const int port = settings.value(QStringLiteral("server/port"), 8123).toString().toInt(&valid);
    if (!valid || port < 1 || port > 65535)
    {
        *error = QStringLiteral("Invalid server port");
        return false;
    }
    config->port = quint16(port);
    const QString max_cells_text = settings.value(QStringLiteral("server/max_cells"), 10000000).toString();
    const qulonglong max_cells = max_cells_text.toULongLong(&valid);
    if (!valid || max_cells < 4 || max_cells > 100000000)
    {
        *error = QStringLiteral("max_cells must be between 4 and 100000000");
        return false;
    }
    config->maximum_cells = std::uint64_t(max_cells);
    const qulonglong coupled_max = settings.value(QStringLiteral("server/max_coupled_dem_cells"),
        10000000).toString().toULongLong(&valid);
    if (!valid || coupled_max < 4 || coupled_max > 100000000)
    {
        *error = QStringLiteral("max_coupled_dem_cells must be between 4 and 100000000");
        return false;
    }
    config->maximum_coupled_dem_cells = std::uint64_t(coupled_max);
    config->max_concurrent_analyses = settings.value(
        QStringLiteral("server/max_concurrent_analyses"), 1).toString().toInt(&valid);
    if (!valid || config->max_concurrent_analyses < 1 ||
        config->max_concurrent_analyses > 4)
    {
        *error = QStringLiteral("server/max_concurrent_analyses must be between 1 and 4");
        return false;
    }
    config->max_outstanding_analyses = settings.value(
        QStringLiteral("server/max_outstanding_analyses"), 8).toString().toInt(&valid);
    if (!valid || config->max_outstanding_analyses < config->max_concurrent_analyses ||
        config->max_outstanding_analyses > 64)
    {
        *error = QStringLiteral("server/max_outstanding_analyses must be between max_concurrent_analyses and 64");
        return false;
    }
    const qulonglong active_cells = settings.value(
        QStringLiteral("server/max_active_terrain_cells"), 0).toString().toULongLong(&valid);
    if (!valid || active_cells > 400000000ULL || (active_cells != 0 && active_cells < 4))
    {
        *error = QStringLiteral("max_active_terrain_cells must be 0 or between 4 and 400000000");
        return false;
    }
    config->max_active_terrain_cells = std::uint64_t(active_cells);
    auto readHttpInt = [&](const QString &key, int fallback, int minimum, int maximum,
                           int *target) -> bool {
        bool parsed = false;
        const int value = settings.value(QStringLiteral("server/") + key, fallback)
                              .toString().toInt(&parsed);
        if (!parsed || value < minimum || value > maximum) {
            *error = QStringLiteral("Invalid server/%1 (valid range %2..%3)")
                         .arg(key).arg(minimum).arg(maximum);
            return false;
        }
        *target = value;
        return true;
    };
    if (!readHttpInt(QStringLiteral("http_workers"), 8, 1, 64, &config->http_workers) ||
        !readHttpInt(QStringLiteral("http_max_queued_requests"), 32, 1, 1024,
                     &config->http_max_queued_requests) ||
        !readHttpInt(QStringLiteral("http_read_timeout_s"), 15, 1, 600,
                     &config->http_read_timeout_s) ||
        !readHttpInt(QStringLiteral("http_write_timeout_s"), 30, 1, 600,
                     &config->http_write_timeout_s) ||
        !readHttpInt(QStringLiteral("http_keep_alive_timeout_s"), 5, 1, 120,
                     &config->http_keep_alive_timeout_s))
        return false;
    if (!readHttpInt(QStringLiteral("max_concurrent_heavy_queries"), 2, 1, 16,
                     &config->max_concurrent_heavy_queries))
        return false;
    const qulonglong hierarchy_basins = settings.value(
        QStringLiteral("server/hierarchy_cache_basins"), 4000000).toString().toULongLong(&valid);
    if (!valid || hierarchy_basins > 100000000ULL)
    {
        *error = QStringLiteral("server/hierarchy_cache_basins must be between 0 and 100000000");
        return false;
    }
    config->hierarchy_cache_basins = std::uint64_t(hierarchy_basins);
    const QString api_key = settings.value(QStringLiteral("server/api_key")).toString();
    if (!api_key.isEmpty() &&
        (api_key.size() < 16 || api_key.size() > 256 ||
         !std::all_of(api_key.cbegin(), api_key.cend(), [](QChar character)
             { return character.unicode() > 0x20 && character.unicode() < 0x7f; })))
    {
        *error = QStringLiteral("server/api_key must be 16..256 printable ASCII characters without spaces");
        return false;
    }
    config->api_key = api_key.toLatin1();
    const QString allow_remote = settings.value(
        QStringLiteral("server/allow_unauthenticated_remote"), QStringLiteral("false"))
        .toString().trimmed().toLower();
    if (allow_remote != QStringLiteral("true") && allow_remote != QStringLiteral("false"))
    {
        *error = QStringLiteral("server/allow_unauthenticated_remote must be true or false");
        return false;
    }
    // The HTTP API can submit, cancel and read every analysis. Never expose
    // it beyond this host without a key unless that is explicitly intended
    // (for example behind an authenticating reverse proxy).
    if (!config->listen_address.isLoopback() && config->api_key.isEmpty() &&
        allow_remote != QStringLiteral("true"))
    {
        *error = QStringLiteral("Non-loopback server/listen_address requires server/api_key "
                                "(or allow_unauthenticated_remote=true behind an authenticating proxy)");
        return false;
    }
    const QString output_path = settings.value(QStringLiteral("server/output_directory")).toString().trimmed();
    if (!output_path.isEmpty())
        config->output_directory = QFileInfo(output_path).isAbsolute()
            ? QDir::cleanPath(output_path) : relative_directory.absoluteFilePath(output_path);
    config->dataset = settings.value(QStringLiteral("terrain/dataset"),
                                    QStringLiteral("copernicus-glo30")).toString();
    config->source.map_url = QUrl(settings.value(QStringLiteral("terrain/map_url"),
                                      QStringLiteral("http://127.0.0.1:8122/")).toString());
    config->source.map_read_api_key = settings.value(
        QStringLiteral("terrain/map_read_api_key")).toString().toUtf8();
    config->source.request_timeout_ms = settings.value(
        QStringLiteral("terrain/request_timeout_ms"), 180000).toString().toInt(&valid);
    if (!valid || config->source.request_timeout_ms < 1000 ||
        config->source.request_timeout_ms > 600000)
    {
        *error = QStringLiteral("Invalid terrain/request_timeout_ms");
        return false;
    }
    config->source.patch_size = settings.value(
        QStringLiteral("terrain/patch_size"), 256).toString().toInt(&valid);
    if (!valid || config->source.patch_size < 1 || config->source.patch_size > 512)
    {
        *error = QStringLiteral("Invalid terrain/patch_size");
        return false;
    }
    config->source.max_concurrent_requests = settings.value(
        QStringLiteral("terrain/max_concurrent_requests"), 4).toString().toInt(&valid);
    if (!valid || config->source.max_concurrent_requests < 1 ||
        config->source.max_concurrent_requests > 4)
    {
        *error = QStringLiteral("Invalid terrain/max_concurrent_requests (valid range 1..4)");
        return false;
    }
    const QString mode = settings.value(QStringLiteral("terrain/mode"),
                                         QStringLiteral("rest")).toString();
    if (mode != QStringLiteral("rest"))
    {
        *error = QStringLiteral("Hydrology server terrain/mode must be rest; standalone uses the library API");
        return false;
    }
    if (!config->source.map_url.isValid() ||
        (config->source.map_url.scheme() != QStringLiteral("http") &&
         config->source.map_url.scheme() != QStringLiteral("https")) ||
        config->source.map_url.host().isEmpty() ||
        !config->source.map_url.userInfo().isEmpty() ||
        !config->source.map_url.query().isEmpty() ||
        !config->source.map_url.fragment().isEmpty())
    {
        *error = QStringLiteral("Invalid map server URL");
        return false;
    }
    if (settings.status() != QSettings::NoError)
    {
        *error = QStringLiteral("Failed to parse hydrology configuration file");
        return false;
    }
    return true;
}
}

int main(int argc, char *argv[])
{
#if defined(Q_OS_UNIX)
    sigset_t termination_signals;
    blockTerminationSignals(&termination_signals);
#endif
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("aowis-server-hydrology"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("AOWIS terrain-based hydrology analysis server"));
    parser.addHelpOption();
    parser.addOption(QCommandLineOption({QStringLiteral("c"), QStringLiteral("config")},
        QStringLiteral("Hydrology server INI file"), QStringLiteral("path")));
    parser.addOption(QCommandLineOption(QStringLiteral("check-config"),
        QStringLiteral("Validate server configuration without starting")));
    parser.addOption(QCommandLineOption(QStringLiteral("print-default-config"),
        QStringLiteral("Print a documented sample configuration")));
    parser.addOption(QCommandLineOption(QStringLiteral("write-default-config"),
        QStringLiteral("Write the default INI to the user data directory, or to an optional positional path")));
    parser.addOption(QCommandLineOption(QStringLiteral("overwrite"),
        QStringLiteral("Explicitly replace an existing example configuration")));
    // Keep the former --write-default-config=PATH spelling valid even though
    // the path is now optional and the Qt option itself takes no value.
    QStringList arguments = application.arguments();
    const QString write_config_prefix = QStringLiteral("--write-default-config=");
    for (int index = 1; index < arguments.size(); ++index)
    {
        if (arguments.at(index).startsWith(write_config_prefix))
        {
            const QString path = arguments.at(index).mid(write_config_prefix.size());
            arguments[index] = QStringLiteral("--write-default-config");
            arguments.insert(index + 1, path);
            ++index;
        }
    }
    parser.process(arguments);
    QTextStream output(stdout);
    QTextStream errors(stderr);
    if (parser.isSet(QStringLiteral("print-default-config")))
    {
        output << exampleConfig();
        return 0;
    }
    const QString user_data_directory = QDir(QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation)).filePath(
            QStringLiteral("aowis-server-hydrology"));
    const QString default_config = QDir(user_data_directory).filePath(
        QStringLiteral("hydrology.ini"));
    if (parser.isSet(QStringLiteral("write-default-config")))
    {
        const QStringList positional_paths = parser.positionalArguments();
        if (positional_paths.size() > 1)
        {
            errors << "Expected at most one configuration output path.\n";
            return 2;
        }
        const QString output_path = positional_paths.isEmpty()
            ? default_config : positional_paths.first();
        if (QFileInfo::exists(output_path) && !parser.isSet(QStringLiteral("overwrite")))
        {
            errors << "Refusing to overwrite configuration; use --overwrite explicitly.\n";
            return 2;
        }
        const QFileInfo output_info(output_path);
        if (!QDir().mkpath(output_info.absolutePath()))
        {
            errors << "Cannot create configuration directory: " << output_info.absolutePath() << '\n';
            return 2;
        }
        QSaveFile destination(output_path);
        if (!destination.open(QIODevice::WriteOnly) ||
            destination.write(exampleConfig().toUtf8()) < 0 || !destination.commit())
        {
            errors << "Cannot write requested configuration file: " << output_path << '\n';
            return 2;
        }
        output << "Wrote " << output_path << '\n';
        return 0;
    }
    if (!parser.positionalArguments().isEmpty())
    {
        errors << "Unexpected positional argument (use --config <path> to load an INI).\n";
        return 2;
    }
    Aowis::Hydrology::ServerConfig config;
    config.output_directory = QDir(user_data_directory).filePath(
        QStringLiteral("analyses"));
    const bool explicit_config = parser.isSet(QStringLiteral("config"));
    const QString config_path = explicit_config
        ? parser.value(QStringLiteral("config")) : default_config;
    QString error;
    if (!loadConfig(config_path, explicit_config, &config, &error))
    {
        errors << error << '\n';
        return 2;
    }

    if (parser.isSet(QStringLiteral("check-config")))
    {
        output << "Hydrology configuration valid: " << config_path << '\n';
        return 0;
    }
#if defined(Q_OS_UNIX)
    startTerminationWatcher(termination_signals);
#endif
    Aowis::Hydrology::Server server(config);
    if (!server.start(&error))
    {
        errors << error << '\n';
        return 1;
    }
    output << "Hydrology server listening on " << config.listen_address.toString()
           << ':' << config.port << '\n';
    output.flush();
    const int status = application.exec();
    output << "Hydrology server stopping\n";
    output.flush();
    return status;
}
