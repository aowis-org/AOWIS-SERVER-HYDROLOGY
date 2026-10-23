#include <aowis/hydrology/service.h>
#include <aowis/hydrology/coupled_routing.h>
#include <aowis/hydrology/depression_archive.h>
#include <aowis/hydrology/spill_geometry.h>
#include <aowis/hydrology/temporal_inflows.h>
#include <aowis/hydrology/travel_time_inflows.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QSaveFile>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Aowis::Hydrology
{
namespace
{
constexpr int MaximumReservoirs = 64;
constexpr int MaximumConnections = 128;
constexpr int MaximumOutlets = 64;
constexpr int MaximumIntervals = 1000;
constexpr int MaximumCurvePoints = 1024;
constexpr qsizetype MaximumRequestBytes = 256 * 1024;
constexpr qint64 ArchiveHeaderBytes = 64;
constexpr qint64 SnapshotChecksumBytes = 32;

void requireSimulation(bool condition, const char *message)
{
    if (!condition) throw std::invalid_argument(message);
}

bool validUuid(const QString &id)
{
    const QUuid parsed(QStringLiteral("{") + id + QStringLiteral("}"));
    return !parsed.isNull() && parsed.toString(QUuid::WithoutBraces) == id;
}

double number(const QJsonObject &json, const char *name)
{
    const QJsonValue value = json.value(QLatin1String(name));
    requireSimulation(value.isDouble() && std::isfinite(value.toDouble()),
        "Invalid coupled simulation numeric parameter");
    return value.toDouble();
}

std::uint64_t indexNumber(const QJsonObject &json, const char *name)
{
    const double value = number(json, name);
    requireSimulation(value >= 0 && value <= 100000000 && std::floor(value) == value,
        "Invalid coupled simulation component index");
    return std::uint64_t(value);
}

QJsonArray boundedArray(const QJsonObject &json, const char *name, int minimum, int maximum)
{
    const QJsonValue value = json.value(QLatin1String(name));
    requireSimulation(value.isArray(), "Invalid coupled simulation array");
    const QJsonArray result = value.toArray();
    requireSimulation(result.size() >= minimum && result.size() <= maximum,
        "Invalid coupled simulation array size");
    return result;
}

std::vector<StageStoragePoint> parseCurve(const QJsonObject &object)
{
    const QJsonArray points = boundedArray(object, "stage_storage", 2, MaximumCurvePoints);
    std::vector<StageStoragePoint> result;
    result.reserve(std::size_t(points.size()));
    for (const QJsonValue &value : points)
    {
        requireSimulation(value.isArray(), "Invalid stage-storage point");
        const QJsonArray pair = value.toArray();
        requireSimulation(pair.size() == 2 && pair.at(0).isDouble() &&
            pair.at(1).isDouble() && std::isfinite(pair.at(0).toDouble()) &&
            std::isfinite(pair.at(1).toDouble()), "Invalid stage-storage point");
        result.push_back({pair.at(0).toDouble(), pair.at(1).toDouble()});
    }
    return result;
}

struct SimulationInput
{
    std::vector<CoupledReservoir> reservoirs;
    std::vector<CoupledConnection> connections;
    std::vector<CoupledBoundaryOutlet> outlets;
    std::vector<CoupledInflowInterval> intervals;
    CoupledRoutingOptions options;
};

SimulationInput parseInput(const QJsonObject &request, const SpillGeometry *geometry,
                           bool scenario_forcing = false)
{
    SimulationInput parsed;
    const QJsonArray reservoirs = boundedArray(request, "reservoirs", 1, MaximumReservoirs);
    for (const QJsonValue &value : reservoirs)
    {
        requireSimulation(value.isObject(), "Invalid reservoir definition");
        const QJsonObject json = value.toObject();
        CoupledReservoir item;
        item.component_index = indexNumber(json, "component_index");
        item.initial_storage_m3 = number(json, "initial_storage_m3");
        item.curve.points = parseCurve(json);
        for (const CoupledReservoir &previous : parsed.reservoirs)
            requireSimulation(previous.component_index != item.component_index,
                "Duplicate reservoir component index");
        parsed.reservoirs.push_back(std::move(item));
    }
    const QJsonArray connections = boundedArray(request, "connections", 0, MaximumConnections);
    if (!connections.isEmpty() && geometry == nullptr)
        throw std::runtime_error("Coupled simulation geometry not loaded");
    for (const QJsonValue &value : connections)
    {
        requireSimulation(value.isObject(), "Invalid hydraulic connection");
        const QJsonObject json = value.toObject();
        const std::uint64_t parent = indexNumber(json, "junction_parent_index");
        const std::vector<BasinSpillJunction>::const_iterator junction =
            std::find_if(geometry->junctions.begin(), geometry->junctions.end(),
                [parent](const BasinSpillJunction &candidate)
                { return candidate.parent_basin_index == parent; });
        requireSimulation(junction != geometry->junctions.end(),
            "Hydraulic control junction not present in the original DEM");
        CoupledConnection item;
        item.junction = *junction;
        const std::uint64_t first = indexNumber(json, "first_component_index");
        const std::uint64_t second = indexNumber(json, "second_component_index");
        const QString kind = json.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("weir"))
        {
            item.type = CoupledControlType::CalibratedBroadCrestedWeir;
            item.weir.first_component_index = first;
            item.weir.second_component_index = second;
            item.weir.crest_elevation_m = number(json, "crest_elevation_m");
            item.weir.effective_width_m = number(json, "effective_width_m");
            item.weir.discharge_factor = number(json, "discharge_factor");
            const QJsonArray rating = boundedArray(json, "submergence_rating", 2, 128);
            for (const QJsonValue &point : rating)
            {
                requireSimulation(point.isArray(), "Invalid submergence rating");
                const QJsonArray pair = point.toArray();
                requireSimulation(pair.size() == 2 && pair.at(0).isDouble() &&
                    pair.at(1).isDouble() && std::isfinite(pair.at(0).toDouble()) &&
                    std::isfinite(pair.at(1).toDouble()), "Invalid submergence rating");
                item.submergence.points.push_back({pair.at(0).toDouble(), pair.at(1).toDouble()});
            }
        }
        else if (kind == QStringLiteral("orifice"))
        {
            item.type = CoupledControlType::FullySubmergedRectangularOrifice;
            item.orifice.first_component_index = first;
            item.orifice.second_component_index = second;
            item.orifice.invert_elevation_m = number(json, "invert_elevation_m");
            item.orifice.opening_width_m = number(json, "opening_width_m");
            item.orifice.opening_height_m = number(json, "opening_height_m");
            item.orifice.discharge_factor = number(json, "discharge_factor");
        }
        else throw std::invalid_argument("Invalid hydraulic control kind");
        parsed.connections.push_back(std::move(item));
    }
    const QJsonArray outlets = boundedArray(request, "outlets", 0, MaximumOutlets);
    for (const QJsonValue &value : outlets)
    {
        requireSimulation(value.isObject(), "Invalid boundary outlet");
        const QJsonObject json = value.toObject();
        CoupledBoundaryOutlet item;
        item.reservoir_component_index = indexNumber(json, "component_index");
        item.crest_elevation_m = number(json, "crest_elevation_m");
        item.effective_width_m = number(json, "effective_width_m");
        item.discharge_factor = number(json, "discharge_factor");
        item.downstream_water_level_m = number(json, "downstream_water_level_m");
        parsed.outlets.push_back(item);
    }
    const QJsonArray intervals = scenario_forcing ? QJsonArray() :
        boundedArray(request, "inflows", 1, MaximumIntervals);
    for (const QJsonValue &value : intervals)
    {
        requireSimulation(value.isObject(), "Invalid temporal inflow interval");
        const QJsonObject json = value.toObject();
        CoupledInflowInterval interval;
        interval.duration_seconds = number(json, "duration_seconds");
        const QJsonArray volumes = boundedArray(json, "volume_m3", int(parsed.reservoirs.size()),
            int(parsed.reservoirs.size()));
        for (const QJsonValue &volume : volumes)
        {
            requireSimulation(volume.isDouble() && std::isfinite(volume.toDouble()) &&
                volume.toDouble() >= 0, "Invalid coupled inflow volume");
            interval.inflow_volume_m3.push_back(volume.toDouble());
        }
        parsed.intervals.push_back(std::move(interval));
    }
    const QJsonObject options = request.value(QStringLiteral("options")).toObject();
    if (options.contains(QStringLiteral("maximum_substep_seconds")))
        parsed.options.maximum_substep_seconds = number(options, "maximum_substep_seconds");
    if (options.contains(QStringLiteral("maximum_substeps")))
        parsed.options.maximum_substeps = indexNumber(options, "maximum_substeps");
    parsed.options.maximum_output_intervals = MaximumIntervals;
    return parsed;
}

// Scenario forcing is opt-in; absence of a forcing object preserves the
// existing, independently prescribed inflow history and binary result format.
struct ScenarioForcingInput
{
    QString scenario_id;
    QString model;
    std::vector<TemporalRainfallInterval> profile;
    TravelTimeOptions travel;
};

ScenarioForcingInput parseScenarioForcing(const QJsonObject &request)
{
    requireSimulation(request.value(QStringLiteral("scenario_forcing")).isObject() &&
        !request.contains(QStringLiteral("inflows")),
        "Specify either scenario_forcing or inflows, never both");
    const QJsonObject forcing = request.value(QStringLiteral("scenario_forcing")).toObject();
    ScenarioForcingInput result;
    result.scenario_id = forcing.value(QStringLiteral("scenario_id")).toString();
    requireSimulation(validUuid(result.scenario_id), "Invalid forcing scenario UUID");
    result.model = forcing.value(QStringLiteral("delivery_model")).toString();
    requireSimulation(result.model == QStringLiteral("instant") ||
        result.model == QStringLiteral("d8_constant_velocity"),
        "Unsupported forcing delivery model");
    const QJsonArray profile = boundedArray(forcing, "time_profile", 1, MaximumIntervals);
    long double fraction_total = 0.0L;
    for (const QJsonValue &value : profile)
    {
        requireSimulation(value.isObject(), "Invalid runoff timing profile interval");
        const QJsonObject interval = value.toObject();
        const double duration = number(interval, "duration_seconds");
        const double fraction = number(interval, "runoff_fraction");
        requireSimulation(duration > 0.0 && fraction >= 0.0 && fraction <= 1.0,
            "Invalid runoff timing profile duration or fraction");
        result.profile.push_back({duration, fraction});
        fraction_total += fraction;
    }
    requireSimulation(std::abs(fraction_total - 1.0L) <= 1e-9L,
        "Runoff timing fractions must sum to one");
    if (result.model == QStringLiteral("d8_constant_velocity"))
    {
        result.travel.effective_velocity_m_per_s = number(forcing, "effective_velocity_m_per_s");
        result.travel.output_interval_seconds = number(forcing, "output_interval_seconds");
        requireSimulation(result.travel.effective_velocity_m_per_s > 0.0 &&
            result.travel.output_interval_seconds > 0.0,
            "Invalid effective runoff velocity or output interval");
        result.travel.maximum_output_intervals = MaximumIntervals;
    }
    else
    {
        requireSimulation(!forcing.contains(QStringLiteral("effective_velocity_m_per_s")) &&
            !forcing.contains(QStringLiteral("output_interval_seconds")),
            "Instant forcing does not use travel-time parameters");
    }
    return result;
}

QString simulationFolder(const QString &root, const QString &id)
{
    return QDir(root).filePath(QStringLiteral("coupled-simulations/") + id);
}

QJsonObject readManifest(const QString &folder)
{
    QFile file(QDir(folder).filePath(QStringLiteral("simulation.json")));
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > MaximumRequestBytes + 4096)
        throw std::runtime_error("Unknown coupled simulation");
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject() || document.object().value(QStringLiteral("format_version")).toInt() != 1)
        throw std::runtime_error("Invalid coupled simulation manifest");
    return document.object();
}

void writeCoupledSimulationManifest(const QString &folder, const QJsonObject &json)
{
    QSaveFile file(QDir(folder).filePath(QStringLiteral("simulation.json")));
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot create coupled simulation manifest");
    const QByteArray bytes = QJsonDocument(json).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot commit coupled simulation manifest");
}

QByteArray requestDigest(const QJsonObject &request)
{
    return QCryptographicHash::hash(QJsonDocument(request).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256);
}

void appendDouble(QByteArray &bytes, double value)
{
    quint64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    char buffer[8];
    qToLittleEndian(bits, buffer);
    bytes.append(buffer, 8);
}

double readDouble(const char *location)
{
    const quint64 bits = qFromLittleEndian<quint64>(location);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(bits));
    if (!std::isfinite(value)) throw std::runtime_error("Invalid coupled simulation snapshot value");
    return value;
}

struct ArchiveShape
{
    std::uint32_t reservoirs = 0;
    std::uint32_t connections = 0;
    std::uint32_t outlets = 0;
    std::uint32_t snapshots = 0;
    qint64 row_bytes = 0;
};

ArchiveShape loadShape(QFile &file, const QByteArray &digest)
{
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Coupled simulation result not ready");
    const QByteArray header = file.read(ArchiveHeaderBytes);
    if (header.size() != ArchiveHeaderBytes || header.left(8) != QByteArrayLiteral("AOWHTS01") ||
        qFromLittleEndian<quint32>(header.constData() + 8) != 1 ||
        header.mid(32, 32) != digest)
        throw std::runtime_error("Invalid coupled simulation result header");
    ArchiveShape shape;
    shape.reservoirs = qFromLittleEndian<quint32>(header.constData() + 12);
    shape.connections = qFromLittleEndian<quint32>(header.constData() + 16);
    shape.outlets = qFromLittleEndian<quint32>(header.constData() + 20);
    shape.snapshots = qFromLittleEndian<quint32>(header.constData() + 24);
    if (shape.reservoirs == 0 || shape.reservoirs > MaximumReservoirs ||
        shape.connections > MaximumConnections || shape.outlets > MaximumOutlets ||
        shape.snapshots < 2 || shape.snapshots > MaximumIntervals + 1 ||
        qFromLittleEndian<quint32>(header.constData() + 28) != 0)
        throw std::runtime_error("Invalid coupled simulation result dimensions");
    shape.row_bytes = SnapshotChecksumBytes + 8 * (5 + qint64(shape.reservoirs) * 2 +
        shape.connections + shape.outlets);
    if (file.size() != ArchiveHeaderBytes + shape.row_bytes * shape.snapshots)
        throw std::runtime_error("Truncated or oversized coupled simulation result");
    return shape;
}

void writeResult(const QString &folder, const QJsonObject &request,
                 const CoupledRoutingResult &result, const std::atomic<bool> &cancelled)
{
    requireSimulation(!result.snapshots.empty(), "Empty coupled simulation result");
    const CoupledRoutingSnapshot &first = result.snapshots.front();
    const std::uint32_t reservoir_count = std::uint32_t(first.storage_m3.size());
    const std::uint32_t connection_count = std::uint32_t(first.connection_transfer_m3.size());
    const std::uint32_t outlet_count = std::uint32_t(first.boundary_outflow_m3.size());
    requireSimulation(result.snapshots.size() >= 2 && result.snapshots.size() <= MaximumIntervals + 1 &&
        reservoir_count > 0 && reservoir_count <= MaximumReservoirs &&
        connection_count <= MaximumConnections && outlet_count <= MaximumOutlets,
        "Invalid coupled simulation result size");
    QSaveFile file(QDir(folder).filePath(QStringLiteral("series-v1.aowbin")));
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot create coupled simulation result");
    QByteArray header(32, '\0');
    std::memcpy(header.data(), "AOWHTS01", 8);
    qToLittleEndian<quint32>(1, header.data() + 8);
    qToLittleEndian<quint32>(reservoir_count, header.data() + 12);
    qToLittleEndian<quint32>(connection_count, header.data() + 16);
    qToLittleEndian<quint32>(outlet_count, header.data() + 20);
    qToLittleEndian<quint32>(quint32(result.snapshots.size()), header.data() + 24);
    header.append(requestDigest(request));
    if (file.write(header) != header.size())
        throw std::runtime_error("Cannot write coupled simulation header");
    for (const CoupledRoutingSnapshot &snapshot : result.snapshots)
    {
        if (cancelled.load()) throw std::runtime_error("Coupled simulation canceled");
        if (snapshot.storage_m3.size() != reservoir_count ||
            snapshot.water_level_m.size() != reservoir_count ||
            snapshot.connection_transfer_m3.size() != connection_count ||
            snapshot.boundary_outflow_m3.size() != outlet_count)
            throw std::runtime_error("Inconsistent coupled snapshot dimensions");
        QByteArray row;
        appendDouble(row, snapshot.elapsed_seconds);
        appendDouble(row, snapshot.cumulative_inflow_m3);
        appendDouble(row, snapshot.cumulative_boundary_outflow_m3);
        appendDouble(row, snapshot.total_storage_m3);
        appendDouble(row, snapshot.mass_balance_error_m3);
        for (double value : snapshot.storage_m3) appendDouble(row, value);
        for (double value : snapshot.water_level_m) appendDouble(row, value);
        for (double value : snapshot.connection_transfer_m3) appendDouble(row, value);
        for (double value : snapshot.boundary_outflow_m3) appendDouble(row, value);
        const QByteArray hash = QCryptographicHash::hash(row, QCryptographicHash::Sha256);
        if (file.write(hash) != hash.size() || file.write(row) != row.size())
            throw std::runtime_error("Cannot write coupled simulation snapshot");
    }
    if (cancelled.load()) throw std::runtime_error("Coupled simulation canceled");
    if (!file.commit()) throw std::runtime_error("Cannot commit coupled simulation result");
}

QJsonArray doublesJson(const QByteArray &row, qsizetype *cursor, std::uint32_t count)
{
    QJsonArray result;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        result.append(readDouble(row.constData() + *cursor));
        *cursor += 8;
    }
    return result;
}

QJsonObject readPage(const QString &folder, const QJsonObject &manifest, int offset, int limit)
{
    QFile file(QDir(folder).filePath(QStringLiteral("series-v1.aowbin")));
    const ArchiveShape shape = loadShape(file, requestDigest(manifest.value(QStringLiteral("request")).toObject()));
    const std::uint32_t first = std::min(std::uint32_t(offset), shape.snapshots);
    const std::uint32_t end = std::min(shape.snapshots, first + std::uint32_t(limit));
    if (!file.seek(ArchiveHeaderBytes + qint64(first) * shape.row_bytes))
        throw std::runtime_error("Cannot seek coupled simulation result");
    QJsonArray snapshots;
    for (std::uint32_t i = first; i < end; ++i)
    {
        const QByteArray record = file.read(shape.row_bytes);
        if (record.size() != shape.row_bytes)
            throw std::runtime_error("Truncated coupled simulation snapshot");
        const QByteArray row = record.mid(SnapshotChecksumBytes);
        if (QCryptographicHash::hash(row, QCryptographicHash::Sha256) !=
            record.left(SnapshotChecksumBytes))
            throw std::runtime_error("Corrupt coupled simulation snapshot");
        qsizetype cursor = 0;
        const double elapsed = readDouble(row.constData() + cursor); cursor += 8;
        const double inflow = readDouble(row.constData() + cursor); cursor += 8;
        const double outflow = readDouble(row.constData() + cursor); cursor += 8;
        const double stored = readDouble(row.constData() + cursor); cursor += 8;
        const double residual = readDouble(row.constData() + cursor); cursor += 8;
        const QJsonArray storage = doublesJson(row, &cursor, shape.reservoirs);
        const QJsonArray levels = doublesJson(row, &cursor, shape.reservoirs);
        const QJsonArray transfers = doublesJson(row, &cursor, shape.connections);
        const QJsonArray outlets = doublesJson(row, &cursor, shape.outlets);
        snapshots.append(QJsonObject{
            {QStringLiteral("interval_index"), int(i)},
            {QStringLiteral("elapsed_seconds"), elapsed},
            {QStringLiteral("storage_m3"), storage},
            {QStringLiteral("water_level_m"), levels},
            {QStringLiteral("connection_cumulative_transfer_m3"), transfers},
            {QStringLiteral("boundary_cumulative_outflow_m3"), outlets},
            {QStringLiteral("cumulative_inflow_m3"), inflow},
            {QStringLiteral("cumulative_boundary_outflow_m3"), outflow},
            {QStringLiteral("total_storage_m3"), stored},
            {QStringLiteral("mass_balance_error_m3"), residual}
        });
    }
    return QJsonObject{
        {QStringLiteral("simulation_id"), manifest.value(QStringLiteral("id"))},
        {QStringLiteral("model"), QStringLiteral("coupled_reservoir_routing_v1")},
        {QStringLiteral("total_snapshots"), int(shape.snapshots)},
        {QStringLiteral("offset"), offset}, {QStringLiteral("limit"), limit},
        {QStringLiteral("has_more"), end < shape.snapshots},
        {QStringLiteral("snapshots"), snapshots},
        {QStringLiteral("scenario_forcing_result"),
            manifest.value(QStringLiteral("scenario_forcing_result"))}
    };
}

ElevationGrid loadDem(const QString &job_folder, const QJsonObject &analysis,
                      std::uint64_t maximum_cells, const std::atomic<bool> *cancelled)
{
    ElevationGrid grid;
    grid.columns = std::uint32_t(analysis.value(QStringLiteral("columns")).toInt());
    grid.rows = std::uint32_t(analysis.value(QStringLiteral("rows")).toInt());
    grid.geometry.origin_x = analysis.value(QStringLiteral("west")).toDouble();
    grid.geometry.origin_y = analysis.value(QStringLiteral("north")).toDouble();
    grid.geometry.pixel_width = analysis.value(QStringLiteral("step_lon_deg")).toDouble();
    grid.geometry.pixel_height = analysis.value(QStringLiteral("step_lat_deg")).toDouble();
    grid.geometry.vertical_datum = analysis.value(QStringLiteral("vertical_datum")) ==
        QStringLiteral("EGM2008") ? VerticalDatum::Egm2008 : VerticalDatum::Unknown;
    const std::uint64_t count = std::uint64_t(grid.columns) * grid.rows;
    requireSimulation(count > 0 && count <= maximum_cells, "Coupled DEM cell limit exceeded");
    QFile file(QDir(job_folder).filePath(QStringLiteral("elevation.f32")));
    if (!file.open(QIODevice::ReadOnly) || file.size() != qint64(count * 4))
        throw std::runtime_error("Missing original elevation raster");
    grid.elevations_m.resize(std::size_t(count));
    QByteArray buffer(16384, '\0');
    for (std::uint64_t pos = 0; pos < count;)
    {
        if (cancelled->load()) throw std::runtime_error("Coupled simulation canceled");
        const qsizetype bytes = qsizetype(std::min<std::uint64_t>(4096, count - pos) * 4);
        if (file.read(buffer.data(), bytes) != bytes)
            throw std::runtime_error("Truncated elevation raster");
        for (qsizetype b = 0; b < bytes; b += 4)
        {
            const quint32 bits = qFromLittleEndian<quint32>(buffer.constData() + b);
            float elevation = 0.0f;
            std::memcpy(&elevation, &bits, 4);
            grid.elevations_m[std::size_t(pos++)] = elevation;
        }
    }
    return grid;
}

SpillGeometry loadGeometry(const QString &folder, const QJsonObject &analysis,
                           std::uint64_t maximum_cells, const std::atomic<bool> *cancelled)
{
    const ElevationGrid grid = loadDem(folder, analysis, maximum_cells, cancelled);
    QFile file(QDir(folder).filePath(QStringLiteral("basins.aowbin")));
    if (!file.open(QIODevice::ReadOnly) || file.size() < 41 ||
        file.size() > 41 + 162 * qint64(grid.columns) * grid.rows ||
        file.size() > std::numeric_limits<int>::max())
        throw std::runtime_error("Missing depression hierarchy");
    const QByteArray content = file.readAll();
    const std::vector<std::uint8_t> bytes(
        reinterpret_cast<const std::uint8_t *>(content.constData()),
        reinterpret_cast<const std::uint8_t *>(content.constData()) + content.size());
    const DepressionHierarchy hierarchy = decodeDepressionHierarchy(bytes, grid.columns, grid.rows);
    return extractSpillGeometry(grid, hierarchy, 2000000, 16000000, cancelled);
}

DepressionHierarchy loadSavedHierarchy(const QString &folder, std::uint32_t columns,
                                       std::uint32_t rows, const std::atomic<bool> *cancelled)
{
    if (cancelled->load()) throw std::runtime_error("Coupled simulation canceled");
    QFile file(QDir(folder).filePath(QStringLiteral("basins.aowbin")));
    const qint64 maximum = 41 + 162 * qint64(columns) * rows;
    if (!file.open(QIODevice::ReadOnly) || file.size() < 41 ||
        file.size() > maximum || file.size() > std::numeric_limits<int>::max())
        throw std::runtime_error("Missing saved basin hierarchy");
    const QByteArray bytes = file.readAll();
    if (cancelled->load()) throw std::runtime_error("Coupled simulation canceled");
    return decodeDepressionHierarchy(std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(bytes.constData()),
        reinterpret_cast<const std::uint8_t *>(bytes.constData()) + bytes.size()),
        columns, rows);
}

std::vector<double> readRunoffRaster(const QString &folder, std::uint64_t cell_count,
                                    const std::atomic<bool> *cancelled)
{
    QFile file(QDir(folder).filePath(QStringLiteral("runoff.f64")));
    if (!file.open(QIODevice::ReadOnly) || file.size() != qint64(cell_count * 8))
        throw std::runtime_error("Missing or invalid rainfall runoff raster");
    std::vector<double> volumes(std::size_t(cell_count), 0.0);
    QByteArray block(8192, '\0');
    for (std::uint64_t position = 0; position < cell_count;)
    {
        if (cancelled->load()) throw std::runtime_error("Coupled simulation canceled");
        const qsizetype byte_count = qsizetype(std::min<std::uint64_t>(1024, cell_count - position) * 8);
        if (file.read(block.data(), byte_count) != byte_count)
            throw std::runtime_error("Truncated rainfall runoff raster");
        for (qsizetype offset = 0; offset < byte_count; offset += 8)
        {
            const quint64 bits = qFromLittleEndian<quint64>(block.constData() + offset);
            double amount = 0.0;
            std::memcpy(&amount, &bits, 8);
            requireSimulation(std::isfinite(amount) && amount >= 0.0,
                "Invalid rainfall runoff raster value");
            volumes[std::size_t(position++)] = amount;
        }
    }
    return volumes;
}

QString fileSha256(const QString &path, const std::atomic<bool> *cancelled)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Coupled forcing source is missing");
    QCryptographicHash digest(QCryptographicHash::Sha256);
    while (!file.atEnd())
    {
        if (cancelled->load()) throw std::runtime_error("Coupled simulation canceled");
        const QByteArray block = file.read(65536);
        if (block.isEmpty() && !file.atEnd())
            throw std::runtime_error("Cannot read coupled forcing source");
        digest.addData(block);
    }
    return QString::fromLatin1(digest.result().toHex());
}

struct PreparedForcing
{
    std::vector<CoupledInflowInterval> intervals;
    QJsonObject provenance;
};

PreparedForcing buildScenarioForcing(const QString &job_folder, const QJsonObject &analysis,
                                    const QJsonObject &request,
                                    const std::vector<CoupledReservoir> &reservoirs,
                                    std::uint64_t maximum_cells,
                                    const std::atomic<bool> *cancelled)
{
    const ScenarioForcingInput input = parseScenarioForcing(request);
    const QString scenario_folder = QDir(job_folder).filePath(
        QStringLiteral("rainfall-scenarios/") + input.scenario_id);
    const QString scenario_path = QDir(scenario_folder).filePath(QStringLiteral("scenario.json"));
    QFile file(scenario_path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 262144)
        throw std::runtime_error("Unknown or invalid rainfall scenario");
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    requireSimulation(document.isObject(), "Invalid rainfall scenario manifest");
    const QJsonObject scenario = document.object();
    requireSimulation(scenario.value(QStringLiteral("id")) == input.scenario_id &&
        scenario.value(QStringLiteral("duration_seconds")).isDouble() &&
        scenario.value(QStringLiteral("effective_runoff_volume_m3")).isDouble(),
        "Inconsistent rainfall scenario identity or quantities");
    const double duration = number(scenario, "duration_seconds");
    const double expected_runoff = number(scenario, "effective_runoff_volume_m3");
    requireSimulation(duration > 0.0 && expected_runoff >= 0.0,
        "Invalid saved rainfall scenario duration or runoff volume");
    const ElevationGrid elevation = loadDem(job_folder, analysis, maximum_cells, cancelled);
    const DepressionHierarchy hierarchy = loadSavedHierarchy(job_folder, elevation.columns,
                                                               elevation.rows, cancelled);
    const std::vector<double> runoff = readRunoffRaster(scenario_folder,
        std::uint64_t(elevation.columns) * elevation.rows, cancelled);
    PreparedForcing prepared;
    double maximum_delay = 0.0;
    double timeline_seconds = duration;
    TemporalRunoffForcing forcing;
    if (input.model == QStringLiteral("instant"))
    {
        forcing = temporalInflowsFromRaster(elevation, hierarchy, runoff, reservoirs,
                                            input.profile, duration, cancelled);
    }
    else
    {
        const TravelTimeRunoffForcing delayed = travelTimeInflowsFromRaster(
            elevation, hierarchy, runoff, reservoirs, input.profile, duration,
            input.travel, cancelled);
        forcing = delayed.forcing;
        maximum_delay = delayed.maximum_source_travel_seconds;
        timeline_seconds = delayed.output_duration_seconds;
    }
    requireSimulation(forcing.basin_inflows.size() >= 1 &&
        forcing.basin_inflows.size() <= MaximumIntervals &&
        forcing.direct_boundary_outflow_m3_by_interval.size() == forcing.basin_inflows.size(),
        "Coupled simulation forcing exceeds the permitted interval budget");
    requireSimulation(std::abs(forcing.generated_runoff_m3 - expected_runoff) <=
        1e-7 * std::max(1.0, expected_runoff),
        "Rainfall runoff file does not match scenario manifest");
    prepared.intervals = std::move(forcing.basin_inflows);
    const QJsonObject sources{
        {QStringLiteral("scenario_manifest_sha256"), fileSha256(scenario_path, cancelled)},
        {QStringLiteral("runoff_raster_sha256"), fileSha256(
            QDir(scenario_folder).filePath(QStringLiteral("runoff.f64")), cancelled)},
        {QStringLiteral("original_dem_sha256"), fileSha256(
            QDir(job_folder).filePath(QStringLiteral("elevation.f32")), cancelled)},
        {QStringLiteral("basin_hierarchy_sha256"), fileSha256(
            QDir(job_folder).filePath(QStringLiteral("basins.aowbin")), cancelled)}
    };
    QJsonObject provenance{
        {QStringLiteral("scenario_id"), input.scenario_id},
        {QStringLiteral("delivery_model"), input.model},
        {QStringLiteral("event_duration_seconds"), duration},
        {QStringLiteral("output_duration_seconds"), timeline_seconds},
        {QStringLiteral("maximum_source_travel_seconds"), maximum_delay},
        {QStringLiteral("runoff_generated_m3"), forcing.generated_runoff_m3},
        {QStringLiteral("delivered_to_basins_m3"), forcing.delivered_to_basins_m3},
        {QStringLiteral("direct_dem_boundary_runoff_m3"), forcing.direct_boundary_outflow_m3},
        {QStringLiteral("mass_balance_error_m3"), forcing.mass_balance_error_m3},
        {QStringLiteral("interval_count"), int(prepared.intervals.size())},
        {QStringLiteral("source_sha256"), sources},
        {QStringLiteral("timing_assumed"), true}
    };
    if (input.model == QStringLiteral("d8_constant_velocity"))
    {
        provenance.insert(QStringLiteral("effective_velocity_m_per_s"),
                          input.travel.effective_velocity_m_per_s);
        provenance.insert(QStringLiteral("output_interval_seconds"),
                          input.travel.output_interval_seconds);
    }
    prepared.provenance = provenance;
    return prepared;
}

QJsonObject taskResponse(const QJsonObject &manifest)
{
    return QJsonObject{
        {QStringLiteral("simulation_id"), manifest.value(QStringLiteral("id"))},
        {QStringLiteral("job_id"), manifest.value(QStringLiteral("job_id"))},
        {QStringLiteral("state"), manifest.value(QStringLiteral("state"))},
        {QStringLiteral("error"), manifest.value(QStringLiteral("error"))},
        {QStringLiteral("result_ready"), manifest.value(QStringLiteral("state")) == QStringLiteral("completed")},
        {QStringLiteral("model"), QStringLiteral("coupled_reservoir_routing_v1")},
        {QStringLiteral("accepted_substeps"), manifest.value(QStringLiteral("accepted_substeps"))},
        {QStringLiteral("rejected_substeps"), manifest.value(QStringLiteral("rejected_substeps"))},
        {QStringLiteral("scenario_forcing_result"),
            manifest.value(QStringLiteral("scenario_forcing_result"))}
    };
}
}

QJsonObject AnalysisService::startCoupledSimulation(const QString &id,
    const QJsonObject &request, QString *error)
{
    if (error != nullptr) error->clear();
    const QJsonObject analysis = this->job(id);
    if (analysis.isEmpty() || analysis.value(QStringLiteral("state")) != QStringLiteral("completed"))
    {
        if (error != nullptr) *error = QStringLiteral("Unknown or incomplete job");
        return {};
    }
    try
    {
        const QByteArray request_bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
        requireSimulation(request_bytes.size() <= MaximumRequestBytes,
            "Invalid coupled simulation request size");
        // Validate the schema without loading the DEM on the request thread.
        // Connections are checked by the background worker against real junctions.
        const QJsonObject without_connections = [&request]()
        {
            QJsonObject copy = request;
            copy.insert(QStringLiteral("connections"), QJsonArray());
            return copy;
        }();
        const QJsonArray requested_connections =
            boundedArray(request, "connections", 0, MaximumConnections);
        if (!requested_connections.isEmpty() || request.contains(QStringLiteral("scenario_forcing")))
        {
            const double columns = analysis.value(QStringLiteral("columns")).toDouble();
            const double rows = analysis.value(QStringLiteral("rows")).toDouble();
            requireSimulation(columns >= 2 && rows >= 2 &&
                columns * rows <= double(this->config.maximum_coupled_dem_cells),
                "Coupled DEM cell limit exceeded");
        }
        requireSimulation(!request.contains(QStringLiteral("options")) ||
            request.value(QStringLiteral("options")).isObject(),
            "Invalid coupled simulation options");
        const bool from_scenario = request.contains(QStringLiteral("scenario_forcing"));
        if (from_scenario) (void)parseScenarioForcing(request);
        (void)parseInput(without_connections, nullptr, from_scenario);
        QSharedPointer<CoupledTask> task;
        {
            QMutexLocker locker(&this->coupled_tasks_mutex);
            std::size_t active = 0;
            for (const QSharedPointer<CoupledTask> &entry : std::as_const(this->coupled_tasks))
                if (entry->state == QStringLiteral("queued") ||
                    entry->state == QStringLiteral("running") ||
                    entry->state == QStringLiteral("cancel_requested")) ++active;
            if (active >= 8)
                throw std::runtime_error("Coupled simulation queue full (8 active/queued)");
            task = QSharedPointer<CoupledTask>::create();
            task->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            task->analysis_id = id;
            task->request = request;
            this->coupled_tasks.insert(id + QLatin1Char('/') + task->id, task);
        }
        const QString folder = simulationFolder(QDir(this->config.output_directory).filePath(id), task->id);
        try
        {
            if (!QDir().mkpath(folder))
                throw std::runtime_error("Cannot create coupled simulation directory");
            writeCoupledSimulationManifest(folder, QJsonObject{
                {QStringLiteral("format_version"), 1},
                {QStringLiteral("id"), task->id},
                {QStringLiteral("job_id"), id},
                {QStringLiteral("state"), QStringLiteral("queued")},
                {QStringLiteral("error"), QString()},
                {QStringLiteral("request"), request}
            });
        }
        catch (...)
        {
            QMutexLocker locker(&this->coupled_tasks_mutex);
            this->coupled_tasks.remove(id + QLatin1Char('/') + task->id);
            throw;
        }
        this->coupled_workers.start([this, task]() { runCoupledTask(task); });
        return coupledSimulationStatus(id, task->id, error);
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

void AnalysisService::runCoupledTask(const QSharedPointer<CoupledTask> &task)
{
    const QString folder = simulationFolder(
        QDir(this->config.output_directory).filePath(task->analysis_id), task->id);
    QString final_state = QStringLiteral("completed");
    QString message;
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    {
        QMutexLocker locker(&this->coupled_tasks_mutex);
        if (!task->cancelled.load()) task->state = QStringLiteral("running");
    }
    try
    {
        if (task->cancelled.load()) throw std::runtime_error("Coupled simulation canceled");
        const QJsonObject analysis = this->job(task->analysis_id);
        requireSimulation(analysis.value(QStringLiteral("state")) == QStringLiteral("completed"),
            "Analysis no longer available");
        const QJsonArray connections = task->request.value(QStringLiteral("connections")).toArray();
        SpillGeometry geometry;
        if (!connections.isEmpty())
            geometry = loadGeometry(QDir(this->config.output_directory).filePath(task->analysis_id),
                                    analysis, this->config.maximum_coupled_dem_cells, &task->cancelled);
        SimulationInput input = parseInput(task->request, &geometry,
            task->request.contains(QStringLiteral("scenario_forcing")));
        QJsonObject forcing_provenance;
        if (task->request.contains(QStringLiteral("scenario_forcing")))
        {
            PreparedForcing prepared = buildScenarioForcing(
                QDir(this->config.output_directory).filePath(task->analysis_id),
                analysis, task->request, input.reservoirs,
                this->config.maximum_coupled_dem_cells, &task->cancelled);
            input.intervals = std::move(prepared.intervals);
            forcing_provenance = prepared.provenance;
        }
        const CoupledRoutingResult result = routeCoupledReservoirsWithOutlets(
            input.reservoirs, input.connections, input.outlets, input.intervals,
            input.options, &task->cancelled);
        accepted = result.accepted_substeps;
        rejected = result.rejected_substeps;
        writeResult(folder, task->request, result, task->cancelled);
        if (!forcing_provenance.isEmpty())
        {
            // Save provenance before completed state is published. A failed
            // write cannot claim a complete, reproducible simulation.
            QJsonObject manifest = readManifest(folder);
            manifest.insert(QStringLiteral("scenario_forcing_result"), forcing_provenance);
            writeCoupledSimulationManifest(folder, manifest);
        }
    }
    catch (const std::exception &ex)
    {
        final_state = task->cancelled.load() ? QStringLiteral("canceled") : QStringLiteral("failed");
        message = QString::fromUtf8(ex.what());
    }
    if (task->cancelled.load()) final_state = QStringLiteral("canceled");
    try
    {
        QJsonObject manifest = readManifest(folder);
        manifest.insert(QStringLiteral("state"), final_state);
        manifest.insert(QStringLiteral("error"), message);
        manifest.insert(QStringLiteral("accepted_substeps"), double(accepted));
        manifest.insert(QStringLiteral("rejected_substeps"), double(rejected));
        writeCoupledSimulationManifest(folder, manifest);
    }
    catch (const std::exception &)
    {
        final_state = QStringLiteral("failed");
        message = QStringLiteral("Failed to persist coupled simulation status");
    }
    QMutexLocker locker(&this->coupled_tasks_mutex);
    task->state = final_state;
    task->error = message;
}

QJsonObject AnalysisService::coupledSimulationStatus(const QString &id,
    const QString &simulation_id, QString *error) const
{
    if (error != nullptr) error->clear();
    if (this->job(id).isEmpty() || !validUuid(simulation_id))
    {
        if (error != nullptr) *error = QStringLiteral("Unknown coupled simulation");
        return {};
    }
    try
    {
        const QString folder = simulationFolder(QDir(this->config.output_directory).filePath(id), simulation_id);
        QJsonObject manifest = readManifest(folder);
        requireSimulation(manifest.value(QStringLiteral("id")) == simulation_id &&
            manifest.value(QStringLiteral("job_id")) == id,
            "Invalid coupled simulation identity");
        {
            QMutexLocker locker(&this->coupled_tasks_mutex);
            const QSharedPointer<CoupledTask> active = this->coupled_tasks.value(
                id + QLatin1Char('/') + simulation_id);
            if (active)
            {
                manifest.insert(QStringLiteral("state"), active->state);
                manifest.insert(QStringLiteral("error"), active->error);
            }
            else if (manifest.value(QStringLiteral("state")) == QStringLiteral("queued") ||
                     manifest.value(QStringLiteral("state")) == QStringLiteral("running"))
            {
                manifest.insert(QStringLiteral("state"), QStringLiteral("interrupted"));
                manifest.insert(QStringLiteral("error"),
                                QStringLiteral("Server restarted before simulation completed"));
            }
        }
        if (manifest.value(QStringLiteral("state")) == QStringLiteral("completed"))
        {
            QFile result(QDir(folder).filePath(QStringLiteral("series-v1.aowbin")));
            (void)loadShape(result, requestDigest(manifest.value(QStringLiteral("request")).toObject()));
        }
        return taskResponse(manifest);
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

QJsonObject AnalysisService::coupledSimulationSnapshots(const QString &id,
    const QString &simulation_id, int offset, int limit, QString *error) const
{
    if (error != nullptr) error->clear();
    if (offset < 0 || limit < 1 || limit > 100)
    {
        if (error != nullptr) *error = QStringLiteral("Invalid coupled simulation pagination (limit 1..100)");
        return {};
    }
    const QJsonObject status = coupledSimulationStatus(id, simulation_id, error);
    if (status.isEmpty()) return {};
    if (status.value(QStringLiteral("state")) != QStringLiteral("completed"))
    {
        if (error != nullptr) *error = QStringLiteral("Coupled simulation result not ready");
        return {};
    }
    try
    {
        const QString folder = simulationFolder(QDir(this->config.output_directory).filePath(id), simulation_id);
        return readPage(folder, readManifest(folder), offset, limit);
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

bool AnalysisService::cancelCoupledSimulation(const QString &id,
    const QString &simulation_id, QString *error)
{
    if (error != nullptr) error->clear();
    QMutexLocker locker(&this->coupled_tasks_mutex);
    const QSharedPointer<CoupledTask> task = this->coupled_tasks.value(
        id + QLatin1Char('/') + simulation_id);
    if (!task || (task->state != QStringLiteral("queued") &&
                  task->state != QStringLiteral("running") &&
                  task->state != QStringLiteral("cancel_requested")))
    {
        if (error != nullptr) *error = QStringLiteral("No active coupled simulation");
        return false;
    }
    task->cancelled.store(true);
    task->state = QStringLiteral("cancel_requested");
    return true;
}
} // namespace Aowis::Hydrology
