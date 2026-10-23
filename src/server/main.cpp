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
; Independent analysis workers (1..4). Default 1 to limit memory use.
max_concurrent_analyses=1
; Maximum queued plus active terrain jobs (1..64, at least worker count).
max_outstanding_analyses=8

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
    Aowis::Hydrology::Server server(config);
    if (!server.start(&error))
    {
        errors << error << '\n';
        return 1;
    }
    output << "Hydrology server listening on " << config.listen_address.toString()
           << ':' << config.port << '\n';
    output.flush();
    return application.exec();
}
