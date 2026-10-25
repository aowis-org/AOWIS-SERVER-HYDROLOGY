#include <aowis/hydrology/service.h>
#include <aowis/hydrology/depression_archive.h>
#include <aowis/hydrology/inundation.h>
#include <aowis/hydrology/redistribution.h>
#include <aowis/hydrology/redistribution_archive.h>
#include <aowis/hydrology/spatial_queries.h>

#include "service_limits.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QByteArrayView>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QHash>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QStringList>
#include <QSaveFile>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <csignal>
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace Aowis::Hydrology
{
struct AnalysisService::Job
{
    QString id;
    QString state = QStringLiteral("queued");
    QString error;
    QString dataset;
    ElevationGrid grid;
    AnalysisOptions options;
    std::uint64_t conditioned_cells = 0;
    float maximum_fill_m = 0.0f;
    std::uint64_t potential_fill_cells = 0;
    double potential_fill_volume_m3 = 0.0;
    float maximum_potential_fill_m = 0.0f;
    std::uint64_t breached_cells = 0;
    std::uint32_t accepted_breach_paths = 0;
    std::uint32_t rejected_breach_candidates = 0;
    double breach_excavation_m3 = 0.0;
    float maximum_breach_cut_m = 0.0f;
    std::uint64_t basin_count = 0;
    QJsonObject stage_timings_ms;
    QJsonObject terrain_fetch_details;
    QJsonObject artifact_integrity;
    // "verified" (published or hashed in this process), "pending" (restored
    // with a SHA-256 inventory, background check not finished) or "legacy"
    // (pre-inventory analysis: size and hierarchy checksum only).
    QString artifact_verification = QStringLiteral("verified");
    std::chrono::steady_clock::time_point queued_at{};
    std::atomic<bool> cancelled {false};
};

struct AnalysisService::RainfallTask
{
    QString id;
    QString analysis_id;
    QJsonObject request;
    QString state = QStringLiteral("queued");
    QString error;
    QJsonObject result;
    std::atomic<bool> cancelled {false};
};

struct AnalysisService::InundationTask
{
    QString id;
    QString analysis_id;
    QString scenario_id;
    QString state = QStringLiteral("queued");
    QString error;
    std::atomic<bool> cancelled {false};
};

struct AnalysisService::RedistributionTask
{
    QString id;
    QString analysis_id;
    QString scenario_id;
    QString state = QStringLiteral("queued");
    QString error;
    std::atomic<bool> cancelled {false};
};

// Immutable decoded hierarchy of one completed analysis plus the UUIDv5 ->
// position index, so a basin lookup is O(1) instead of one SHA-1 per basin.
struct CachedBasinHierarchy
{
    DepressionHierarchy hierarchy;
    QHash<QString, std::uint64_t> index_by_uuid;
};

namespace
{
bool safeJobId(const QString &id)
{
    return !id.isEmpty() && id.size() <= 40 &&
        std::all_of(id.cbegin(), id.cend(), [](QChar character)
        {
            return character.isLetterOrNumber() || character == QChar('-');
        });
}

const QString HeavyQueryBusy = QStringLiteral("Heavy query capacity busy; retry later");
const QString VerificationPending =
    QStringLiteral("Analysis integrity verification in progress; retry later");

// Admission for synchronous whole-grid queries. A synchronous caller (no
// cancellation flag) fails fast when all slots are taken; a cancellable
// background worker waits for a slot.
class HeavyQuerySlot final
{
public:
    HeavyQuerySlot(QSemaphore &semaphore, const std::atomic<bool> *cancel_requested)
        : admission(semaphore)
    {
        if (cancel_requested == nullptr)
        {
            this->held = this->admission.tryAcquire();
            return;
        }
        while (!cancel_requested->load())
        {
            if (this->admission.tryAcquire(1, 50))
            {
                this->held = true;
                return;
            }
        }
    }
    ~HeavyQuerySlot()
    {
        if (this->held) this->admission.release();
    }
    HeavyQuerySlot(const HeavyQuerySlot &) = delete;
    HeavyQuerySlot &operator=(const HeavyQuerySlot &) = delete;
    bool acquired() const { return this->held; }

private:
    QSemaphore &admission;
    bool held = false;
};

// Remove catchment spools left behind by processes that no longer exist.
// ".catchment-tasks-<pid>-XXXXXX" belongs to <pid>; it is removed only when
// that process is gone. Spools of older versions carry no PID and are removed
// once untouched for a day. Only transient task masks are ever stored there.
void removeStaleCatchmentSpools(const QString &output_directory)
{
    const QDir output(output_directory);
    const QStringList entries = output.entryList({QStringLiteral(".catchment-tasks-*")},
        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QString &name : entries)
    {
        const QString suffix = name.mid(QStringLiteral(".catchment-tasks-").size());
        const qsizetype dash = suffix.indexOf(QLatin1Char('-'));
        bool numeric = false;
        const qint64 pid = dash > 0 ? suffix.left(dash).toLongLong(&numeric) : 0;
        bool stale = false;
        if (numeric && pid > 0)
        {
            if (pid == QCoreApplication::applicationPid()) continue;
#if defined(Q_OS_UNIX)
            stale = ::kill(pid_t(pid), 0) != 0 && errno == ESRCH;
#endif
        }
        else
        {
            const QDateTime modified = QFileInfo(output.filePath(name)).lastModified();
            stale = modified.isValid() &&
                modified.secsTo(QDateTime::currentDateTime()) > 24 * 3600;
        }
        if (stale)
            QDir(output.filePath(name)).removeRecursively();
    }
}

// IEEE754 and integer arrays are written to stable little-endian files.
// Outputs are immutable for the lifetime of a completed job.
template <typename T>
QString writeArray(const QString &path, const std::vector<T> &values)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error(QStringLiteral("Cannot create analysis result: %1")
            .arg(path).toStdString());
    QByteArray buffer(32768, '\0');
    qsizetype used = 0;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const T &value : values)
    {
        static_assert(sizeof(T) == 4 || sizeof(T) == 8);
        if (used + qsizetype(sizeof(T)) > buffer.size())
        {
            if (file.write(buffer.constData(), used) != used)
                throw std::runtime_error("Cannot write analysis result");
            hash.addData(QByteArrayView(buffer.constData(), used));
            used = 0;
        }
        if constexpr (sizeof(T) == 4)
        {
            quint32 bits = 0;
            std::memcpy(&bits, &value, sizeof(T));
            qToLittleEndian(bits, buffer.data() + used);
        }
        else
        {
            quint64 bits = 0;
            std::memcpy(&bits, &value, sizeof(T));
            qToLittleEndian(bits, buffer.data() + used);
        }
        used += sizeof(T);
    }
    if (used > 0)
    {
        if (file.write(buffer.constData(), used) != used)
            throw std::runtime_error("Cannot write analysis result");
        hash.addData(QByteArrayView(buffer.constData(), used));
    }
    if (!file.commit())
        throw std::runtime_error("Cannot commit analysis result");
    return QString::fromLatin1(hash.result().toHex());
}

struct Layer
{
    QString file;
    int bytes_per_cell = 0;
    QByteArray mime;
};

Layer layerSpec(const QString &layer)
{
    if (layer == QStringLiteral("elevation"))
        return {QStringLiteral("elevation.f32"), 4, QByteArrayLiteral("application/vnd.aowis.raster-f32le")};
    if (layer == QStringLiteral("conditioned"))
        return {QStringLiteral("conditioned.f32"), 4, QByteArrayLiteral("application/vnd.aowis.raster-f32le")};
    if (layer == QStringLiteral("potential-fill-depth"))
        return {QStringLiteral("potential-fill-depth.f32"), 4, QByteArrayLiteral("application/vnd.aowis.raster-f32le")};
    if (layer == QStringLiteral("breach-cut-depth"))
        return {QStringLiteral("breach-cut-depth.f32"), 4, QByteArrayLiteral("application/vnd.aowis.raster-f32le")};
    if (layer == QStringLiteral("upstream-area"))
        return {QStringLiteral("upstream-area.f64"), 8, QByteArrayLiteral("application/vnd.aowis.raster-f64le")};
    if (layer == QStringLiteral("upstream-count"))
        return {QStringLiteral("upstream-count.u64"), 8, QByteArrayLiteral("application/vnd.aowis.raster-u64le")};
    if (layer == QStringLiteral("downstream"))
        return {QStringLiteral("downstream.i64"), 8, QByteArrayLiteral("application/vnd.aowis.raster-i64le")};
    return {};
}

// The seven primary rasters are fixed-size row-major arrays; only basin
// hierarchy is variable length. No rainfall/simulation derived cache is a
// prerequisite for a terrain job to be complete.
struct PrimaryArtifact
{
    const char *name;
    int bytes_per_cell;
};
constexpr std::array<PrimaryArtifact, 8> PrimaryArtifacts{{
    {"elevation.f32", 4}, {"conditioned.f32", 4},
    {"potential-fill-depth.f32", 4}, {"breach-cut-depth.f32", 4},
    {"upstream-area.f64", 8}, {"upstream-count.u64", 8},
    {"downstream.i64", 8}, {"basins.aowbin", 0}
}};

QJsonObject artifactRecord(std::uint64_t bytes, const QString &sha256)
{
    return QJsonObject{{QStringLiteral("size_bytes"), double(bytes)},
                       {QStringLiteral("sha256"), sha256}};
}

// Streaming archive verification avoids decoding potentially hundreds of MB
// of basin records during startup. The existing decoder additionally validates
// semantic basin relationships whenever a basin result is accessed.
bool verifyBasinArchive(QFile &file, std::uint32_t columns, std::uint32_t rows,
                        QString *reason)
{
    const QByteArray header = file.read(33);
    const std::uint64_t cells = std::uint64_t(columns) * rows;
    if (header.size() != 33 || std::memcmp(header.constData(), "AOWHDH01", 8) != 0 ||
        qFromLittleEndian<quint32>(header.constData() + 8) != columns ||
        qFromLittleEndian<quint32>(header.constData() + 12) != rows ||
        qFromLittleEndian<quint64>(header.constData() + 16) > cells ||
        (header.at(24) != 0 && header.at(24) != 1))
    {
        *reason = QStringLiteral("Invalid basin archive header");
        return false;
    }
    const quint64 basin_count = qFromLittleEndian<quint64>(header.constData() + 25);
    if (basin_count > 2 * cells ||
        file.size() != qint64(41 + 81 * basin_count))
    {
        *reason = QStringLiteral("Invalid basin archive length");
        return false;
    }
    constexpr quint64 fnv_prime = 1099511628211ULL;
    quint64 hash = 14695981039346656037ULL;
    for (unsigned char byte : header)
    {
        hash ^= byte;
        hash *= fnv_prime;
    }
    qint64 remaining = file.size() - 33 - 8;
    while (remaining > 0)
    {
        const QByteArray chunk = file.read(std::min<qint64>(remaining, 1024 * 1024));
        if (chunk.isEmpty())
        {
            *reason = QStringLiteral("Truncated basin archive");
            return false;
        }
        for (unsigned char byte : chunk)
        {
            hash ^= byte;
            hash *= fnv_prime;
        }
        remaining -= chunk.size();
    }
    const QByteArray trailer = file.read(8);
    if (trailer.size() != 8 || qFromLittleEndian<quint64>(trailer.constData()) != hash)
    {
        *reason = QStringLiteral("Basin archive checksum mismatch");
        return false;
    }
    return true;
}

// With hash_contents == false only the structure is checked: file presence,
// exact lengths, the basin archive checksum and the inventory format. The
// SHA-256 of every raster (tens of bytes per cell) is compared only when
// hash_contents is true; that pass is cancellable through stop.
bool verifyPrimaryArtifacts(const QString &folder, std::uint32_t columns,
                            std::uint32_t rows, const QJsonObject &identities,
                            QString *reason, bool hash_contents = true,
                            const std::atomic<bool> *stop = nullptr)
{
    const std::uint64_t cells = std::uint64_t(columns) * rows;
    // Validate manifest geometry before doing multiplication, I/O or hashing.
    if (columns < 2 || rows < 2 || columns > 100000 || rows > 100000 ||
        cells > 100000000ULL)
    {
        *reason = QStringLiteral("Invalid completed grid dimensions");
        return false;
    }
    // An absent inventory means a pre-upgrade job. An inventory, if present,
    // must contain all files: partial or malformed inventories are corruption.
    if (!identities.isEmpty() && identities.size() != int(PrimaryArtifacts.size()))
    {
        *reason = QStringLiteral("Incomplete artifact integrity inventory");
        return false;
    }
    for (const PrimaryArtifact &spec : PrimaryArtifacts)
    {
        const QString name = QString::fromLatin1(spec.name);
        const QFileInfo info(QDir(folder).filePath(name));
        if (info.isSymLink() || !info.isFile())
        {
            *reason = QStringLiteral("Missing or invalid artifact: %1").arg(name);
            return false;
        }
        const qint64 expected = spec.bytes_per_cell == 0
            ? -1 : qint64(cells * std::uint64_t(spec.bytes_per_cell));
        if (expected >= 0 && info.size() != expected)
        {
            *reason = QStringLiteral("Incorrect artifact length: %1").arg(name);
            return false;
        }
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly))
        {
            *reason = QStringLiteral("Unreadable artifact: %1").arg(name);
            return false;
        }
        if (spec.bytes_per_cell == 0 && !verifyBasinArchive(file, columns, rows, reason))
            return false;
        if (!identities.isEmpty())
        {
            const QJsonObject entry = identities.value(name).toObject();
            const QString digest = entry.value(QStringLiteral("sha256")).toString();
            if (entry.value(QStringLiteral("size_bytes")).toDouble(-1) != double(info.size()) ||
                digest.size() != 64 ||
                !std::all_of(digest.cbegin(), digest.cend(), [](QChar c) {
                    return (c >= QChar('0') && c <= QChar('9')) ||
                           (c >= QChar('a') && c <= QChar('f'));
                }))
            {
                *reason = QStringLiteral("Invalid artifact identity: %1").arg(name);
                return false;
            }
            if (!hash_contents)
                continue;
            if (!file.seek(0))
            {
                *reason = QStringLiteral("Cannot rewind artifact: %1").arg(name);
                return false;
            }
            QCryptographicHash hash(QCryptographicHash::Sha256);
            while (!file.atEnd())
            {
                if (stop != nullptr && stop->load())
                {
                    *reason = QStringLiteral("Artifact verification interrupted");
                    return false;
                }
                const QByteArray chunk = file.read(1024 * 1024);
                if (chunk.isEmpty())
                {
                    *reason = QStringLiteral("Cannot hash artifact: %1").arg(name);
                    return false;
                }
                hash.addData(chunk);
            }
            if (QString::fromLatin1(hash.result().toHex()) != digest)
            {
                *reason = QStringLiteral("Artifact checksum mismatch: %1").arg(name);
                return false;
            }
        }
    }
    return true;
}

QByteArray readRectangle(const QString &path, std::uint32_t grid_columns,
                         int x, int y, int width, int height, int bytes_per_cell)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot open completed analysis raster");
    QByteArray data(qsizetype(width) * height * bytes_per_cell, '\0');
    for (int row = 0; row < height; ++row)
    {
        const qint64 start = (qint64(y + row) * grid_columns + x) * bytes_per_cell;
        const qsizetype size = qsizetype(width) * bytes_per_cell;
        if (!file.seek(start) || file.read(data.data() + qsizetype(row) * size, size) != size)
            throw std::runtime_error("Completed analysis raster is truncated");
    }
    return data;
}

std::int64_t downstreamValue(const char *ptr)
{
    const quint64 raw = qFromLittleEndian<quint64>(ptr);
    std::int64_t value = 0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

std::uint64_t countValue(const char *ptr)
{
    return qFromLittleEndian<quint64>(ptr);
}

// UUIDv5 makes each basin ID deterministic within its analysis job and
// preserves identity over server restarts. Array offsets are NOT entity IDs.
QString basinUuid(const QString &job_id, std::uint64_t index)
{
    const QUuid namespace_uuid(QStringLiteral("{") + job_id + QStringLiteral("}"));
    if (namespace_uuid.isNull())
        throw std::runtime_error("Invalid analysis UUID");
    return QUuid::createUuidV5(namespace_uuid, QString::number(qulonglong(index)).toUtf8())
        .toString(QUuid::WithoutBraces);
}

DepressionHierarchy loadHierarchy(const QString &folder, std::uint32_t columns,
                                  std::uint32_t rows)
{
    QFile file(QDir(folder).filePath(QStringLiteral("basins.aowbin")));
    const std::uint64_t cells = std::uint64_t(columns) * rows;
    const std::uint64_t maximum_bytes = 41ULL + 162ULL * cells;
    if (!file.open(QIODevice::ReadOnly) || file.size() < 41 ||
        std::uint64_t(file.size()) > maximum_bytes || file.size() > std::numeric_limits<int>::max())
        throw std::runtime_error("Missing or oversized depression hierarchy archive");
    const QByteArray bytes = file.readAll();
    if (bytes.size() != file.size())
        throw std::runtime_error("Failed to read complete depression hierarchy archive");
    return decodeDepressionHierarchy(std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(bytes.constData()),
        reinterpret_cast<const std::uint8_t *>(bytes.constData()) + bytes.size()),
        columns, rows);
}

ElevationGrid loadOriginalElevation(const QString &folder, const ElevationGrid &geometry,
                                    const std::atomic<bool> *cancel_requested = nullptr)
{
    const std::uint64_t cells = std::uint64_t(geometry.columns) * geometry.rows;
    QFile file(QDir(folder).filePath(QStringLiteral("elevation.f32")));
    if (!file.open(QIODevice::ReadOnly) || file.size() != qint64(cells * 4))
        throw std::runtime_error("Missing or incomplete source elevation raster");
    ElevationGrid source = geometry;
    source.elevations_m.resize(std::size_t(cells));
    QByteArray buffer(32768, '\0');
    std::uint64_t cell = 0;
    while (cell < cells)
    {
        if (cancel_requested != nullptr && cancel_requested->load())
            throw std::runtime_error("Runoff redistribution cancelled");
        const qsizetype take = qsizetype(std::min<std::uint64_t>(8192, cells - cell) * 4);
        if (file.read(buffer.data(), take) != take)
            throw std::runtime_error("Truncated source elevation raster");
        for (qsizetype i = 0; i < take; i += 4)
        {
            const quint32 bits = qFromLittleEndian<quint32>(buffer.constData() + i);
            float elevation = 0.0f;
            std::memcpy(&elevation, &bits, sizeof(bits));
            source.elevations_m[std::size_t(cell++)] = elevation;
        }
    }
    return source;
}

const DepressionBasin &findBasin(const CachedBasinHierarchy &cached,
                                 const QString &uuid)
{
    const QUuid requested(QStringLiteral("{") + uuid + QStringLiteral("}"));
    if (requested.isNull())
        throw std::invalid_argument("Invalid basin UUID");
    const QHash<QString, std::uint64_t>::const_iterator found =
        cached.index_by_uuid.constFind(requested.toString(QUuid::WithoutBraces));
    if (found == cached.index_by_uuid.cend())
        throw std::invalid_argument("Unknown basin UUID");
    return cached.hierarchy.basins[std::size_t(found.value())];
}

QJsonObject basinJson(const DepressionHierarchy &hierarchy, const QString &job_id,
                      const DepressionBasin &basin)
{
    QJsonObject json;
    json.insert(QStringLiteral("id"), basinUuid(job_id, basin.index));
    if (basin.parent_index >= 0)
        json.insert(QStringLiteral("parent_id"), basinUuid(job_id, std::uint64_t(basin.parent_index)));
    else
        json.insert(QStringLiteral("parent_id"), QJsonValue::Null);
    QJsonArray children;
    for (const std::uint64_t child : basin.child_indices)
        children.append(basinUuid(job_id, child));
    json.insert(QStringLiteral("child_ids"), children);
    json.insert(QStringLiteral("minimum_x"), int(basin.minimum_cell % hierarchy.columns));
    json.insert(QStringLiteral("minimum_y"), int(basin.minimum_cell / hierarchy.columns));
    json.insert(QStringLiteral("minimum_elevation_m"), basin.minimum_elevation_m);
    json.insert(QStringLiteral("formation_elevation_m"), basin.formation_elevation_m);
    if (std::isfinite(basin.spill_elevation_m))
    {
        json.insert(QStringLiteral("spill_elevation_m"), basin.spill_elevation_m);
        json.insert(QStringLiteral("capacity_at_spill_m3"), basin.capacity_at_spill_m3);
        json.insert(QStringLiteral("area_below_spill_m2"), basin.area_below_spill_m2);
        json.insert(QStringLiteral("cells_below_spill"), double(basin.cells_below_spill));
        json.insert(QStringLiteral("spill_x"), int(basin.spill_cell % hierarchy.columns));
        json.insert(QStringLiteral("spill_y"), int(basin.spill_cell / hierarchy.columns));
    }
    else
    {
        json.insert(QStringLiteral("spill_elevation_m"), QJsonValue::Null);
        json.insert(QStringLiteral("capacity_at_spill_m3"), QJsonValue::Null);
        json.insert(QStringLiteral("area_below_spill_m2"), QJsonValue::Null);
    }
    json.insert(QStringLiteral("spills_to_domain_boundary"), basin.spills_to_domain_boundary);
    json.insert(QStringLiteral("model"), QStringLiteral("static_topographic_storage"));
    return json;
}

QJsonObject stateJson(const BasinStorageState &state)
{
    return QJsonObject{
        {QStringLiteral("water_level_m"), state.water_level_m},
        {QStringLiteral("water_volume_m3"), state.water_volume_m3},
        {QStringLiteral("inundated_cell_area_m2"), state.inundated_cell_area_m2},
        {QStringLiteral("inundated_cells"), double(state.inundated_cells)}
    };
}

bool parseGeographicRing(const QJsonArray &input, GeographicRing *ring,
                         std::uint64_t *total_vertices)
{
    if (input.size() < 4 || input.size() > 2048 ||
        *total_vertices + std::uint64_t(input.size()) > 30000)
        return false;
    ring->clear();
    for (const QJsonValue &value : input)
    {
        if (!value.isArray()) return false;
        const QJsonArray coordinate = value.toArray();
        if (coordinate.size() != 2 || !coordinate[0].isDouble() ||
            !coordinate[1].isDouble()) return false;
        const double longitude = coordinate[0].toDouble();
        const double latitude = coordinate[1].toDouble();
        if (!std::isfinite(longitude) || !std::isfinite(latitude) ||
            longitude < -180.0 || longitude > 180.0 ||
            latitude < -90.0 || latitude > 90.0) return false;
        ring->push_back({longitude, latitude});
    }
    *total_vertices += std::uint64_t(input.size());
    return ring->front().x == ring->back().x &&
           ring->front().y == ring->back().y;
}

bool parseGeographicPolygon(const QJsonArray &rings,
                            GeographicPolygon *polygon, std::uint64_t *vertices)
{
    if (rings.isEmpty() || rings.size() > 65 || !rings[0].isArray() ||
        !parseGeographicRing(rings[0].toArray(), &polygon->exterior, vertices))
        return false;
    for (qsizetype index = 1; index < rings.size(); ++index)
    {
        if (!rings[index].isArray()) return false;
        GeographicRing hole;
        if (!parseGeographicRing(rings[index].toArray(), &hole, vertices))
            return false;
        polygon->holes.push_back(std::move(hole));
    }
    return true;
}

std::vector<GeographicPolygon> parseGeoJsonPolygons(const QJsonObject &object)
{
    QJsonObject geometry = object.value(QStringLiteral("geometry")).toObject();
    if (geometry.isEmpty()) geometry = object;
    const QString type = geometry.value(QStringLiteral("type")).toString();
    const QJsonValue value = geometry.value(QStringLiteral("coordinates"));
    if (!value.isArray())
        throw std::invalid_argument("Invalid polygon GeoJSON coordinates");
    const QJsonArray coordinates = value.toArray();
    std::vector<GeographicPolygon> polygons;
    std::uint64_t total_vertices = 0;
    if (type == QStringLiteral("Polygon"))
    {
        GeographicPolygon polygon;
        if (!parseGeographicPolygon(coordinates, &polygon, &total_vertices))
            throw std::invalid_argument("Invalid GeoJSON polygon or ring");
        polygons.push_back(std::move(polygon));
    }
    else if (type == QStringLiteral("MultiPolygon"))
    {
        if (coordinates.isEmpty() || coordinates.size() > 32)
            throw std::invalid_argument("Invalid number of polygons");
        for (const QJsonValue &value : coordinates)
        {
            if (!value.isArray())
                throw std::invalid_argument("Invalid GeoJSON multipolygon element");
            GeographicPolygon polygon;
            if (!parseGeographicPolygon(value.toArray(), &polygon, &total_vertices))
                throw std::invalid_argument("Invalid GeoJSON multipolygon ring");
            polygons.push_back(std::move(polygon));
        }
    }
    else throw std::invalid_argument("Invalid geometry type: expected Polygon or MultiPolygon");
    return polygons;
}

// Read-only memory map of one persisted fixed-size raster. Queries read only
// the cells they need; untouched pages are never loaded, and touched pages
// are reclaimable page cache rather than per-request heap allocations.
class MappedRaster final
{
public:
    MappedRaster(const QString &path, std::uint64_t expected_bytes, const char *problem)
        : file(path)
    {
        if (!this->file.open(QIODevice::ReadOnly) ||
            std::uint64_t(this->file.size()) != expected_bytes)
            throw std::runtime_error(problem);
        this->data = this->file.map(0, qint64(expected_bytes));
        if (this->data == nullptr)
            throw std::runtime_error(problem);
    }
    MappedRaster(const MappedRaster &) = delete;
    MappedRaster &operator=(const MappedRaster &) = delete;
    const uchar *bytes() const { return this->data; }

private:
    QFile file;
    uchar *data = nullptr;
};

// Stored D8 graph (downstream links + contributing area) through mapped
// readers; nothing proportional to the grid is allocated.
struct StoredFlowGraph
{
    StoredFlowGraph(const ElevationGrid &geometry, const QString &directory)
        : links(QDir(directory).filePath(QStringLiteral("downstream.i64")),
                std::uint64_t(geometry.columns) * geometry.rows * 8,
                "Missing or incomplete stored flow graph"),
          areas(QDir(directory).filePath(QStringLiteral("upstream-area.f64")),
                std::uint64_t(geometry.columns) * geometry.rows * 8,
                "Missing or incomplete stored flow graph")
    {
    }
    DownstreamReader downstream() const
    {
        const uchar *base = this->links.bytes();
        return [base](std::uint64_t cell)
        {
            return downstreamValue(reinterpret_cast<const char *>(base + cell * 8));
        };
    }
    UpstreamAreaReader upstreamArea() const
    {
        const uchar *base = this->areas.bytes();
        return [base](std::uint64_t cell)
        {
            const quint64 bits = qFromLittleEndian<quint64>(base + cell * 8);
            double area = 0.0;
            std::memcpy(&area, &bits, sizeof(area));
            return area;
        };
    }
    MappedRaster links;
    MappedRaster areas;
};

QJsonObject channelNetworkJson(const ChannelNetwork &network,
                               const ElevationGrid &grid)
{
    QJsonArray features;
    for (const ChannelSegment &segment : network.segments)
    {
        QJsonArray coordinates;
        for (std::uint64_t cell : segment.cells)
        {
            const MapCoordinate coordinate = pixelCentreWgs84(grid.geometry,
                std::uint32_t(cell % grid.columns), std::uint32_t(cell / grid.columns));
            coordinates.append(QJsonArray{coordinate.x, coordinate.y});
        }
        features.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("Feature")},
            {QStringLiteral("geometry"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("LineString")},
                {QStringLiteral("coordinates"), coordinates}}},
            {QStringLiteral("properties"), QJsonObject{
                {QStringLiteral("id"), double(segment.id)},
                {QStringLiteral("from_cell"), double(segment.from_cell)},
                {QStringLiteral("to_cell"), double(segment.to_cell)},
                {QStringLiteral("strahler_order"), int(segment.strahler_order)},
                {QStringLiteral("length_m"), segment.length_m},
                {QStringLiteral("upstream_area_at_end_m2"), segment.upstream_area_at_end_m2}}}
        });
    }
    QJsonArray nodes;
    for (const ChannelNode &node : network.nodes)
    {
        const MapCoordinate coordinate = pixelCentreWgs84(grid.geometry,
            std::uint32_t(node.cell_index % grid.columns),
            std::uint32_t(node.cell_index / grid.columns));
        nodes.append(QJsonObject{
            {QStringLiteral("cell_index"), double(node.cell_index)},
            {QStringLiteral("incoming_channels"), int(node.incoming_channels)},
            {QStringLiteral("strahler_order"), int(node.strahler_order)},
            {QStringLiteral("coordinates"), QJsonArray{coordinate.x, coordinate.y}}
        });
    }
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("FeatureCollection")},
        {QStringLiteral("features"), features},
        {QStringLiteral("nodes"), nodes},
        {QStringLiteral("threshold_m2"), network.threshold_m2},
        {QStringLiteral("selected_cells"), double(network.selected_cells)},
        {QStringLiteral("boundary_truncation_possible"), true}
    };
}

} // namespace

AnalysisService::AnalysisService(const ServiceConfig &config)
    : config(config)
{
    this->workers.setMaxThreadCount(1);
    this->rainfall_workers.setMaxThreadCount(2);
    this->catchment_workers.setMaxThreadCount(1);
    this->redistribution_workers.setMaxThreadCount(1);
    this->coupled_workers.setMaxThreadCount(1);
    this->verification_workers.setMaxThreadCount(1);
}

AnalysisService::~AnalysisService()
{
    // Finish any active work before member objects (including the job map and
    // the shared Map source) can be destroyed. QThreadPool alone would be
    // destroyed after the job map due to member destruction order.
    this->stopping.store(true);
    this->verification_workers.waitForDone();
    {
        QMutexLocker locker(&this->jobs_mutex);
        for (const QSharedPointer<Job> &job : std::as_const(this->job_map))
        {
            if (job->state != QStringLiteral("completed") &&
                job->state != QStringLiteral("failed") &&
                job->state != QStringLiteral("canceled"))
                job->cancelled.store(true);
        }
    }
    {
        QMutexLocker locker(&this->rainfall_tasks_mutex);
        for (const QSharedPointer<RainfallTask> &task : std::as_const(this->rainfall_tasks))
            task->cancelled.store(true);
        this->rainfall_queue.clear();
    }
    {
        QMutexLocker locker(&this->catchment_tasks_mutex);
        for (const auto &task : std::as_const(this->catchment_tasks))
            task->cancelled.store(true);
        this->catchment_queue.clear();
    }
    {
        QMutexLocker locker(&this->redistribution_tasks_mutex);
        for (const QSharedPointer<RedistributionTask> &task :
             std::as_const(this->redistribution_tasks))
            task->cancelled.store(true);
        for (const QSharedPointer<InundationTask> &task :
             std::as_const(this->inundation_tasks))
            task->cancelled.store(true);
    }
    {
        QMutexLocker locker(&this->coupled_tasks_mutex);
        for (const QSharedPointer<CoupledTask> &task : std::as_const(this->coupled_tasks))
            task->cancelled.store(true);
    }
    this->rainfall_workers.waitForDone();
    this->catchment_workers.waitForDone();
    this->coupled_workers.waitForDone();
    this->redistribution_workers.waitForDone();
    this->workers.waitForDone();
}

bool AnalysisService::initialize(QString *error)
{
    if (this->config.maximum_cells < 4 ||
        this->config.maximum_cells > 100000000ULL)
    {
        if (error != nullptr)
            *error = QStringLiteral("max_cells must be between 4 and 100000000");
        return false;
    }
    if (this->config.maximum_coupled_dem_cells < 4 ||
        this->config.maximum_coupled_dem_cells > 100000000ULL)
    {
        if (error != nullptr)
            *error = QStringLiteral("max_coupled_dem_cells must be between 4 and 100000000");
        return false;
    }
    if (this->config.max_active_terrain_cells != 0 &&
        (this->config.max_active_terrain_cells < 4 ||
         this->config.max_active_terrain_cells > 400000000ULL))
    {
        if (error != nullptr)
            *error = QStringLiteral("max_active_terrain_cells must be 0 or between 4 and 400000000");
        return false;
    }
    if (this->config.max_concurrent_analyses < 1 ||
        this->config.max_concurrent_analyses > 4)
    {
        if (error != nullptr)
            *error = QStringLiteral("max_concurrent_analyses must be between 1 and 4");
        return false;
    }
    if (this->config.max_outstanding_analyses < this->config.max_concurrent_analyses ||
        this->config.max_outstanding_analyses > 64)
    {
        if (error != nullptr)
            *error = QStringLiteral("max_outstanding_analyses must be between max_concurrent_analyses and 64");
        return false;
    }
    if (this->config.max_concurrent_heavy_queries < 1 ||
        this->config.max_concurrent_heavy_queries > 16)
    {
        if (error != nullptr)
            *error = QStringLiteral("max_concurrent_heavy_queries must be between 1 and 16");
        return false;
    }
    if (this->initialized)
    {
        if (error != nullptr) *error = QStringLiteral("Hydrology service is already initialized");
        return false;
    }
    this->workers.setMaxThreadCount(this->config.max_concurrent_analyses);
    if (this->config.output_directory.isEmpty() ||
        !QDir().mkpath(this->config.output_directory))
    {
        if (error != nullptr)
            *error = QStringLiteral("Cannot create hydrology output directory");
        return false;
    }
    removeStaleCatchmentSpools(this->config.output_directory);
    // Keep transient catchment masks separate from persisted scientific
    // results. QTemporaryDir removes them only after the workers have joined.
    // The owner PID in the name lets a later instance remove the spool of a
    // killed process (whose destructor never ran) without touching the spool
    // of another live service sharing the directory.
    this->catchment_spool = std::make_unique<QTemporaryDir>(
        QDir(this->config.output_directory).filePath(QStringLiteral(".catchment-tasks-%1-XXXXXX")
            .arg(QCoreApplication::applicationPid())));
    if (!this->catchment_spool->isValid())
    {
        if (error != nullptr) *error = QStringLiteral("Cannot create catchment task spool");
        return false;
    }
    // Only a fully successful initialization opens admission; a failed call
    // above may be retried after fixing its cause.
    this->initialized = true;
    this->heavy_query_slots.release(this->config.max_concurrent_heavy_queries);
    restoreJobs();
    this->verification_workers.start([this]() { verifyRestoredArtifacts(); });
    return true;
}

bool AnalysisService::waitForArtifactVerification(int timeout_ms)
{
    return this->verification_workers.waitForDone(timeout_ms);
}

QString AnalysisService::jobDirectory(const QString &id) const
{
    return QDir(this->config.output_directory).filePath(id);
}

QSharedPointer<AnalysisService::Job> AnalysisService::findJob(const QString &id) const
{
    QMutexLocker locker(&this->jobs_mutex);
    return this->job_map.value(id);
}

QSharedPointer<AnalysisService::Job> AnalysisService::completedJob(const QString &id,
                                                                   QString *error) const
{
    const QSharedPointer<Job> item = safeJobId(id) ? findJob(id) : QSharedPointer<Job>();
    QMutexLocker locker(&this->jobs_mutex);
    if (!item || item->state != QStringLiteral("completed"))
    {
        if (error != nullptr) *error = QStringLiteral("Unknown or incomplete job");
        return {};
    }
    if (item->artifact_verification == QStringLiteral("pending"))
    {
        if (error != nullptr) *error = VerificationPending;
        return {};
    }
    return item;
}

std::shared_ptr<const CachedBasinHierarchy> AnalysisService::cachedHierarchy(
    const QSharedPointer<Job> &job) const
{
    {
        QMutexLocker locker(&this->hierarchy_cache_mutex);
        const std::shared_ptr<const CachedBasinHierarchy> cached =
            this->hierarchy_cache.value(job->id);
        if (cached)
        {
            this->hierarchy_cache_order.removeOne(job->id);
            this->hierarchy_cache_order.enqueue(job->id);
            return cached;
        }
    }
    // Decoding plus one UUIDv5 per basin is the expensive part; done once per
    // analysis while the entry stays cached. Completed analyses are immutable.
    std::shared_ptr<CachedBasinHierarchy> loaded = std::make_shared<CachedBasinHierarchy>();
    {
        const HeavyQuerySlot slot(this->heavy_query_slots, nullptr);
        if (!slot.acquired()) throw std::runtime_error(HeavyQueryBusy.toStdString());
        loaded->hierarchy = loadHierarchy(jobDirectory(job->id), job->grid.columns, job->grid.rows);
        loaded->index_by_uuid.reserve(qsizetype(loaded->hierarchy.basins.size()));
        for (const DepressionBasin &basin : loaded->hierarchy.basins)
            loaded->index_by_uuid.insert(basinUuid(job->id, basin.index), basin.index);
    }
    const std::uint64_t size = std::max<std::uint64_t>(1, loaded->hierarchy.basins.size());
    QMutexLocker locker(&this->hierarchy_cache_mutex);
    if (size <= this->config.hierarchy_cache_basins && !this->hierarchy_cache.contains(job->id))
    {
        while (this->hierarchy_cache_size + size > this->config.hierarchy_cache_basins &&
               !this->hierarchy_cache_order.isEmpty())
        {
            const QString evicted = this->hierarchy_cache_order.dequeue();
            const std::shared_ptr<const CachedBasinHierarchy> old =
                this->hierarchy_cache.take(evicted);
            if (old)
                this->hierarchy_cache_size -= std::max<std::uint64_t>(1, old->hierarchy.basins.size());
        }
        this->hierarchy_cache.insert(job->id, loaded);
        this->hierarchy_cache_order.enqueue(job->id);
        this->hierarchy_cache_size += size;
    }
    return loaded;
}

void AnalysisService::verifyRestoredArtifacts()
{
    // Restored analyses with a SHA-256 inventory are hashed here, after the
    // service (and an HTTP listener) is already available. Their data is not
    // served until this check passes; see completedJob().
    QList<QSharedPointer<Job>> pending;
    {
        QMutexLocker locker(&this->jobs_mutex);
        QStringList ids = this->job_map.keys();
        ids.sort();
        for (const QString &id : std::as_const(ids))
        {
            const QSharedPointer<Job> candidate = this->job_map.value(id);
            if (candidate->artifact_verification == QStringLiteral("pending"))
                pending.append(candidate);
        }
    }
    for (const QSharedPointer<Job> &next : std::as_const(pending))
    {
        if (this->stopping.load()) return;
        QString reason;
        const bool valid = verifyPrimaryArtifacts(jobDirectory(next->id), next->grid.columns,
            next->grid.rows, next->artifact_integrity, &reason, true, &this->stopping);
        if (this->stopping.load()) return;
        {
            QMutexLocker locker(&this->jobs_mutex);
            if (valid)
                next->artifact_verification = QStringLiteral("verified");
            else
            {
                next->artifact_verification = QStringLiteral("verified");
                next->state = QStringLiteral("failed");
                next->error = QStringLiteral("Invalid completed analysis artifacts: %1; rerun analysis")
                    .arg(reason);
            }
        }
        if (!valid) writeManifest(next);
    }
}

QJsonObject AnalysisService::jobJson(const QSharedPointer<Job> &job) const
{
    QMutexLocker locker(&this->jobs_mutex);
    QJsonObject result;
    result.insert(QStringLiteral("id"), job->id);
    result.insert(QStringLiteral("state"), job->state);
    result.insert(QStringLiteral("error"), job->error);
    result.insert(QStringLiteral("dataset"), job->dataset);
    result.insert(QStringLiteral("west"), job->grid.geometry.origin_x);
    result.insert(QStringLiteral("north"), job->grid.geometry.origin_y);
    result.insert(QStringLiteral("step_lon_deg"), job->grid.geometry.pixel_width);
    result.insert(QStringLiteral("step_lat_deg"), job->grid.geometry.pixel_height);
    result.insert(QStringLiteral("columns"), int(job->grid.columns));
    result.insert(QStringLiteral("rows"), int(job->grid.rows));
    result.insert(QStringLiteral("horizontal_crs"), QStringLiteral("EPSG:4326"));
    result.insert(QStringLiteral("vertical_datum"),
        job->grid.geometry.vertical_datum == VerticalDatum::Egm2008
            ? QStringLiteral("EGM2008") : QStringLiteral("unknown"));
    result.insert(QStringLiteral("fill_depressions"), job->options.fill_depressions);
    result.insert(QStringLiteral("breach_depressions"), job->options.breach_depressions);
    result.insert(QStringLiteral("maximum_breach_cut_depth_m"), job->options.maximum_breach_cut_depth_m);
    result.insert(QStringLiteral("maximum_breach_path_cells"), int(job->options.maximum_breach_path_cells));
    result.insert(QStringLiteral("maximum_breach_excavation_m3"), job->options.maximum_breach_excavation_m3);
    result.insert(QStringLiteral("maximum_breach_candidates"), int(job->options.maximum_breach_candidates));
    result.insert(QStringLiteral("breached_cells"), double(job->breached_cells));
    result.insert(QStringLiteral("accepted_breach_paths"), int(job->accepted_breach_paths));
    result.insert(QStringLiteral("rejected_breach_candidates"), int(job->rejected_breach_candidates));
    result.insert(QStringLiteral("breach_excavation_m3"), job->breach_excavation_m3);
    result.insert(QStringLiteral("maximum_breach_cut_m"), double(job->maximum_breach_cut_m));
    result.insert(QStringLiteral("conditioned_cells"), double(job->conditioned_cells));
    result.insert(QStringLiteral("maximum_fill_m"), double(job->maximum_fill_m));
    result.insert(QStringLiteral("potential_fill_cells"), double(job->potential_fill_cells));
    result.insert(QStringLiteral("potential_fill_volume_m3"), job->potential_fill_volume_m3);
    result.insert(QStringLiteral("maximum_potential_fill_m"), double(job->maximum_potential_fill_m));
    result.insert(QStringLiteral("basin_count"), double(job->basin_count));
    result.insert(QStringLiteral("stage_timings_ms"), job->stage_timings_ms);
    if (!job->terrain_fetch_details.isEmpty())
        result.insert(QStringLiteral("terrain_fetch_details"), job->terrain_fetch_details);
    if (!job->artifact_integrity.isEmpty())
        result.insert(QStringLiteral("artifact_integrity"), job->artifact_integrity);
    if (job->state == QStringLiteral("completed"))
        result.insert(QStringLiteral("artifact_verification"), job->artifact_verification);
    return result;
}

bool AnalysisService::writeManifest(const QSharedPointer<Job> &job,
                                    const QString &state_override) const
{
    QSaveFile file(QDir(jobDirectory(job->id)).filePath(QStringLiteral("job.json")));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QJsonObject manifest = jobJson(job);
    if (!state_override.isEmpty())
        manifest.insert(QStringLiteral("state"), state_override);
    const QByteArray data = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    return file.write(data) == data.size() && file.commit();
}

void AnalysisService::restoreJobs()
{
    const QDir output(this->config.output_directory);
    for (const QString &id : output.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        if (!safeJobId(id))
            continue;
        QFile file(output.filePath(id + QStringLiteral("/job.json")));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (!document.isObject())
            continue;
        const QJsonObject saved = document.object();
        // The directory, not untrusted JSON, defines the identity. Do not
        // resurrect a renamed/mismatched manifest under another job UUID.
        if (saved.value(QStringLiteral("id")).toString() != id)
            continue;
        QSharedPointer<Job> job = QSharedPointer<Job>::create();
        job->id = id;
        job->dataset = saved.value(QStringLiteral("dataset")).toString();
        job->state = saved.value(QStringLiteral("state")).toString();
        job->error = saved.value(QStringLiteral("error")).toString();
        job->grid.columns = std::uint32_t(saved.value(QStringLiteral("columns")).toInt());
        job->grid.rows = std::uint32_t(saved.value(QStringLiteral("rows")).toInt());
        job->grid.geometry.origin_x = saved.value(QStringLiteral("west")).toDouble();
        job->grid.geometry.origin_y = saved.value(QStringLiteral("north")).toDouble();
        job->grid.geometry.pixel_width = saved.value(QStringLiteral("step_lon_deg")).toDouble();
        job->grid.geometry.pixel_height = saved.value(QStringLiteral("step_lat_deg")).toDouble();
        job->grid.geometry.vertical_datum = saved.value(QStringLiteral("vertical_datum")) ==
            QStringLiteral("EGM2008") ? VerticalDatum::Egm2008 : VerticalDatum::Unknown;
        job->options.fill_depressions = saved.value(QStringLiteral("fill_depressions")).toBool(true);
        job->options.breach_depressions = saved.value(QStringLiteral("breach_depressions")).toBool(false);
        job->options.maximum_breach_cut_depth_m = saved.value(QStringLiteral("maximum_breach_cut_depth_m")).toDouble(3.0);
        job->options.maximum_breach_path_cells = std::uint32_t(saved.value(QStringLiteral("maximum_breach_path_cells")).toInt(8));
        job->options.maximum_breach_excavation_m3 = saved.value(QStringLiteral("maximum_breach_excavation_m3")).toDouble(100000.0);
        job->options.maximum_breach_candidates = std::uint32_t(saved.value(QStringLiteral("maximum_breach_candidates")).toInt(2000));
        job->breached_cells = std::uint64_t(saved.value(QStringLiteral("breached_cells")).toDouble());
        job->accepted_breach_paths = std::uint32_t(saved.value(QStringLiteral("accepted_breach_paths")).toInt());
        job->rejected_breach_candidates = std::uint32_t(saved.value(QStringLiteral("rejected_breach_candidates")).toInt());
        job->breach_excavation_m3 = saved.value(QStringLiteral("breach_excavation_m3")).toDouble();
        job->maximum_breach_cut_m = float(saved.value(QStringLiteral("maximum_breach_cut_m")).toDouble());
        job->conditioned_cells = std::uint64_t(saved.value(QStringLiteral("conditioned_cells")).toDouble());
        job->maximum_fill_m = float(saved.value(QStringLiteral("maximum_fill_m")).toDouble());
        job->potential_fill_cells = std::uint64_t(saved.value(QStringLiteral("potential_fill_cells")).toDouble());
        job->potential_fill_volume_m3 = saved.value(QStringLiteral("potential_fill_volume_m3")).toDouble();
        job->maximum_potential_fill_m = float(saved.value(QStringLiteral("maximum_potential_fill_m")).toDouble());
        job->basin_count = std::uint64_t(saved.value(QStringLiteral("basin_count")).toDouble());
        job->stage_timings_ms = saved.value(QStringLiteral("stage_timings_ms")).toObject();
        job->terrain_fetch_details = saved.value(QStringLiteral("terrain_fetch_details")).toObject();
        job->artifact_integrity = saved.value(QStringLiteral("artifact_integrity")).toObject();
        bool damaged_artifacts = false;
        if (job->state == QStringLiteral("completed"))
        {
            QString reason;
            // Old analyses have no artifact_integrity; they still receive
            // exact-size and hierarchy checksum validation.
            if ((saved.contains(QStringLiteral("artifact_integrity")) &&
                 (!saved.value(QStringLiteral("artifact_integrity")).isObject() ||
                  job->artifact_integrity.isEmpty())) ||
                !verifyPrimaryArtifacts(jobDirectory(id), job->grid.columns,
                                        job->grid.rows, job->artifact_integrity, &reason,
                                        false))
            {
                job->state = QStringLiteral("failed");
                job->error = QStringLiteral("Invalid completed analysis artifacts: %1; rerun analysis")
                    .arg(reason.isEmpty() ? QStringLiteral("Invalid artifact inventory") : reason);
                damaged_artifacts = true;
            }
            else
            {
                // Content hashes (tens of bytes per cell) are checked in the
                // background instead of blocking startup for every job.
                job->artifact_verification = job->artifact_integrity.isEmpty()
                    ? QStringLiteral("legacy") : QStringLiteral("pending");
            }
        }
        if (job->state != QStringLiteral("completed") && job->state != QStringLiteral("failed") &&
            job->state != QStringLiteral("canceled"))
        {
            job->state = QStringLiteral("failed");
            job->error = QStringLiteral("Server interrupted before completing job");
            QMutexLocker locker(&this->jobs_mutex);
            this->job_map.insert(id, job);
            locker.unlock();
            writeManifest(job);
            continue;
        }
        QMutexLocker locker(&this->jobs_mutex);
        this->job_map.insert(id, job);
        locker.unlock();
        if (damaged_artifacts)
            writeManifest(job);
    }
}

void AnalysisService::runJob(const QSharedPointer<Job> &job)
{
    using Clock = std::chrono::steady_clock;
    const Clock::time_point worker_started = Clock::now();
    // This is QUEUE delay, not worker execution: the one-worker baseline
    // serialized jobs even though their Map HTTP patches overlapped.
    if (job->queued_at != Clock::time_point{})
    {
        const double queue_ms = std::chrono::duration<double, std::milli>(
            worker_started - job->queued_at).count();
        QMutexLocker locker(&this->jobs_mutex);
        job->stage_timings_ms.insert(QStringLiteral("queue_wait"),
                                     std::max(0.0, queue_ms));
    }
    // All timing fields represent elapsed monotonic wall time, never CPU time.
    // Updating a job's metrics uses the same lock as jobJson(), so HTTP
    // readers cannot observe a partially updated JSON object.
    const std::function<void(const QString &, const Clock::time_point &)> record_stage =
        [this, &job](const QString &name, const Clock::time_point &started)
        {
            const double duration_ms =
                std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            QMutexLocker locker(&this->jobs_mutex);
            job->stage_timings_ms.insert(name, duration_ms);
        };
    try
    {
        {
            QMutexLocker locker(&this->jobs_mutex);
            // A queued cancellation is already terminal and persisted; do not
            // run or overwrite it once its turn in QThreadPool arrives.
            if (job->state == QStringLiteral("canceled")) return;
            if (job->cancelled.load())
                throw std::runtime_error("Job canceled");
            job->state = QStringLiteral("fetching-terrain");
        }
        writeManifest(job);
        const Clock::time_point fetch_started = Clock::now();
        TerrainFetchMetrics fetch_metrics;
        ElevationGrid source = this->config.terrain_reader
            ? this->config.terrain_reader(job->grid, job->dataset, job->cancelled)
            : readTerrain(this->config.source, job->grid, job->dataset,
                          job->cancelled, &fetch_metrics);
        record_stage(QStringLiteral("terrain_fetch"), fetch_started);
        if (!this->config.terrain_reader && fetch_metrics.patch_count > 0)
        {
            const QJsonObject details{
                {QStringLiteral("transport"), fetch_metrics.transport},
                {QStringLiteral("patch_count"), double(fetch_metrics.patch_count)},
                {QStringLiteral("response_bytes"), double(fetch_metrics.response_bytes)},
                {QStringLiteral("map_request_ms"), fetch_metrics.map_request_ms},
                {QStringLiteral("map_request_wall_ms"), fetch_metrics.map_request_wall_ms},
                {QStringLiteral("peak_in_flight"), fetch_metrics.peak_in_flight},
                {QStringLiteral("raster_copy_ms"), fetch_metrics.raster_copy_ms},
                {QStringLiteral("first_request_ms"), fetch_metrics.first_request_ms},
                {QStringLiteral("fastest_request_ms"), fetch_metrics.fastest_request_ms},
                {QStringLiteral("slowest_request_ms"), fetch_metrics.slowest_request_ms}
            };
            QMutexLocker locker(&this->jobs_mutex);
            job->terrain_fetch_details = details;
        }
        const Clock::time_point validation_started = Clock::now();
        if (source.columns != job->grid.columns || source.rows != job->grid.rows ||
            source.geometry.origin_x != job->grid.geometry.origin_x ||
            source.geometry.origin_y != job->grid.geometry.origin_y ||
            source.geometry.pixel_width != job->grid.geometry.pixel_width ||
            source.geometry.pixel_height != job->grid.geometry.pixel_height ||
            source.geometry.horizontal_crs != job->grid.geometry.horizontal_crs ||
            source.geometry.utm_zone != job->grid.geometry.utm_zone ||
            source.geometry.northern_hemisphere != job->grid.geometry.northern_hemisphere ||
            source.geometry.vertical_datum != job->grid.geometry.vertical_datum)
            throw std::runtime_error("Terrain source returned incompatible grid geometry");
        // A source gap at an outer edge can silently become a false watershed
        // outlet. Reject incomplete input throughout the computation domain.
        std::size_t missing_cells = 0;
        std::size_t first_missing = 0;
        for (std::size_t index = 0; index < source.elevations_m.size(); ++index)
        {
            if ((index & 0xFFFFU) == 0 && job->cancelled.load())
                throw std::runtime_error("Job canceled");
            if (!std::isfinite(source.elevations_m[index]))
            {
                if (missing_cells == 0) first_missing = index;
                ++missing_cells;
            }
        }
        if (missing_cells != 0)
            throw std::runtime_error("DEM coverage incomplete: " +
                std::to_string(missing_cells) + " missing cells, first at x=" +
                std::to_string(first_missing % source.columns) + ", y=" +
                std::to_string(first_missing / source.columns));
        if (job->cancelled.load())
            throw std::runtime_error("Job canceled");
        record_stage(QStringLiteral("terrain_validation"), validation_started);
        {
            QMutexLocker locker(&this->jobs_mutex);
            job->state = QStringLiteral("analyzing");
        }
        writeManifest(job);
        const Clock::time_point analysis_started = Clock::now();
        AnalysisResult result = analyze(source, job->options, &job->cancelled);
        // analyze() keeps its own copy in result.source; release the fetched
        // grid so the job does not hold the elevations twice while saving.
        std::vector<float>().swap(source.elevations_m);
        record_stage(QStringLiteral("d8_analysis"), analysis_started);
        if (job->cancelled.load())
            throw std::runtime_error("Job canceled");
        {
            QMutexLocker locker(&this->jobs_mutex);
            job->state = QStringLiteral("saving");
            job->conditioned_cells = result.conditioned_cell_count;
            job->maximum_fill_m = result.maximum_fill_m;
            job->potential_fill_cells = result.potential_fill_cell_count;
            job->potential_fill_volume_m3 = result.potential_fill_volume_m3;
            job->maximum_potential_fill_m = result.maximum_potential_fill_m;
            job->breached_cells = result.breached_cell_count;
            job->accepted_breach_paths = result.accepted_breach_paths;
            job->rejected_breach_candidates = result.rejected_breach_candidates;
            job->breach_excavation_m3 = result.breach_excavation_m3;
            job->maximum_breach_cut_m = result.maximum_breach_cut_m;
        }
        writeManifest(job);
        const QDir folder(jobDirectory(job->id));
        const Clock::time_point raster_save_started = Clock::now();
        const auto saveRaster = [this, &job, &folder](const QString &name, const auto &values)
        {
            const QString sha256 = writeArray(folder.filePath(name), values);
            const QJsonObject entry = artifactRecord(
                std::uint64_t(values.size()) * sizeof(typename std::decay_t<decltype(values)>::value_type),
                sha256);
            QMutexLocker locker(&this->jobs_mutex);
            job->artifact_integrity.insert(name, entry);
        };
        saveRaster(QStringLiteral("elevation.f32"), result.source.elevations_m);
        saveRaster(QStringLiteral("conditioned.f32"), result.conditioned_elevations_m);
        saveRaster(QStringLiteral("potential-fill-depth.f32"), result.potential_fill_depth_m);
        saveRaster(QStringLiteral("breach-cut-depth.f32"), result.breach_cut_depth_m);
        saveRaster(QStringLiteral("upstream-area.f64"), result.upstream_area_m2);
        saveRaster(QStringLiteral("upstream-count.u64"), result.upstream_cell_count);
        saveRaster(QStringLiteral("downstream.i64"), result.downstream);
        record_stage(QStringLiteral("raster_persistence"), raster_save_started);
        {
            QMutexLocker locker(&this->jobs_mutex);
            job->state = QStringLiteral("analyzing-basins");
        }
        writeManifest(job);
        const Clock::time_point hierarchy_started = Clock::now();
        const DepressionHierarchy hierarchy = buildDepressionHierarchy(
            result.source, true, &job->cancelled);
        record_stage(QStringLiteral("depression_hierarchy"), hierarchy_started);
        if (job->cancelled.load())
            throw std::runtime_error("Job canceled");
        const Clock::time_point basin_save_started = Clock::now();
        const std::vector<std::uint8_t> encoded = encodeDepressionHierarchy(hierarchy);
        QSaveFile archive(folder.filePath(QStringLiteral("basins.aowbin")));
        if (!archive.open(QIODevice::WriteOnly) ||
            archive.write(reinterpret_cast<const char *>(encoded.data()),
                          qsizetype(encoded.size())) != qsizetype(encoded.size()) ||
            !archive.commit())
            throw std::runtime_error("Cannot save depression hierarchy archive");
        QCryptographicHash basin_hash(QCryptographicHash::Sha256);
        basin_hash.addData(QByteArrayView(reinterpret_cast<const char *>(encoded.data()), qsizetype(encoded.size())));
        {
            QMutexLocker locker(&this->jobs_mutex);
            job->artifact_integrity.insert(QStringLiteral("basins.aowbin"),
                artifactRecord(encoded.size(), QString::fromLatin1(basin_hash.result().toHex())));
        }
        record_stage(QStringLiteral("basin_persistence"), basin_save_started);
        {
            QMutexLocker locker(&this->jobs_mutex);
            job->basin_count = hierarchy.basins.size();
            job->state = QStringLiteral("saving");
        }
        writeManifest(job);
        if (job->cancelled.load())
            throw std::runtime_error("Job canceled");
        if (job->artifact_integrity.size() != int(PrimaryArtifacts.size()))
            throw std::runtime_error("Cannot publish incomplete analysis artifacts");
        QString artifact_problem;
        if (!verifyPrimaryArtifacts(folder.path(), job->grid.columns, job->grid.rows,
                                    QJsonObject(), &artifact_problem))
            throw std::runtime_error(QStringLiteral("Cannot publish invalid analysis artifacts: %1")
                .arg(artifact_problem).toStdString());
        record_stage(QStringLiteral("total_worker"), worker_started);
        // Publish a completed manifest atomically while status stays 'saving'.
        // Readers may observe 'completed' only AFTER durable publication.
        if (!writeManifest(job, QStringLiteral("completed")))
            throw std::runtime_error("Could not persist job completion metadata");
        {
            QMutexLocker locker(&this->jobs_mutex);
            job->state = QStringLiteral("completed");
        }
    }
    catch (const std::exception &error)
    {
        record_stage(QStringLiteral("total_worker"), worker_started);
        {
            QMutexLocker locker(&this->jobs_mutex);
            job->state = job->cancelled.load()
                ? QStringLiteral("canceled") : QStringLiteral("failed");
            job->error = QString::fromUtf8(error.what());
        }
        writeManifest(job);
    }
}

QString AnalysisService::submit(const AnalysisRequest &request, QString *error)
{
    if (error != nullptr) error->clear();
    const QString dataset = request.dataset.isEmpty() ? this->config.dataset : request.dataset;
    if (dataset != this->config.dataset ||
        !std::isfinite(request.west) || !std::isfinite(request.south) ||
        !std::isfinite(request.east) || !std::isfinite(request.north) ||
        !std::isfinite(request.resolution_m) ||
        request.west < -180 || request.east > 180 || request.west >= request.east ||
        request.south < -85 || request.north > 85 || request.south >= request.north ||
        request.resolution_m < 30.0 || request.resolution_m > 5000.0)
    {
        if (error != nullptr) *error = QStringLiteral("Invalid terrain dataset, bounds or resolution");
        return {};
    }
    if (request.breach_depressions &&
        (!std::isfinite(request.maximum_breach_cut_depth_m) ||
         request.maximum_breach_cut_depth_m <= 0.0 ||
         !std::isfinite(request.maximum_breach_excavation_m3) ||
         request.maximum_breach_excavation_m3 <= 0.0 ||
         request.maximum_breach_path_cells < 2 ||
         request.maximum_breach_path_cells > 32 ||
         request.maximum_breach_candidates == 0 ||
         request.maximum_breach_candidates > 100000))
    {
        if (error != nullptr) *error = QStringLiteral("Invalid constrained breach limits");
        return {};
    }
    constexpr double metres_per_degree = 111195.08;
    constexpr double radians_per_degree = 0.017453292519943295;
    const double latitude_radians = 0.5 * (request.north + request.south) * radians_per_degree;
    const double columns_d = std::ceil((request.east - request.west) *
        metres_per_degree * std::cos(latitude_radians) / request.resolution_m);
    const double rows_d = std::ceil((request.north - request.south) *
        metres_per_degree / request.resolution_m);
    if (!std::isfinite(columns_d) || !std::isfinite(rows_d) ||
        columns_d < 2 || rows_d < 2 || columns_d > 100000 || rows_d > 100000 ||
        columns_d * rows_d > double(this->config.maximum_cells))
    {
        if (error != nullptr) *error = QStringLiteral("Analysis exceeds configured cell limit or is too small");
        return {};
    }
    // An individual job must be runnable under the aggregate terrain-cell
    // limit; reject before creating a job directory, rather than queueing a
    // task that can never be dispatched. Zero means "use maximum_cells".
    const std::uint64_t active_cell_limit = this->config.max_active_terrain_cells == 0
        ? this->config.maximum_cells : this->config.max_active_terrain_cells;
    if (std::uint64_t(columns_d) * std::uint64_t(rows_d) > active_cell_limit)
    {
        if (error != nullptr)
            *error = QStringLiteral("Terrain analysis exceeds active terrain cell budget (%1 cells)")
                .arg(qulonglong(active_cell_limit));
        return {};
    }
    // Admission covers not only jobs already published in job_map but also
    // other submit() calls preparing manifests. It must be reserved before
    // UUID/directory creation so rejected requests leave no disk artifacts.
    {
        QMutexLocker locker(&this->jobs_mutex);
        int outstanding = this->terrain_admission_reservations;
        for (const QSharedPointer<Job> &entry : std::as_const(this->job_map))
        {
            if (entry->state != QStringLiteral("completed") &&
                entry->state != QStringLiteral("failed") &&
                entry->state != QStringLiteral("canceled")) ++outstanding;
        }
        if (outstanding >= this->config.max_outstanding_analyses)
        {
            if (error != nullptr)
                *error = QStringLiteral("Terrain analysis capacity reached (%1 active/queued jobs)")
                             .arg(this->config.max_outstanding_analyses);
            return {};
        }
        ++this->terrain_admission_reservations;
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QSharedPointer<Job> job = QSharedPointer<Job>::create();
    job->id = id;
    job->dataset = dataset;
    job->grid.columns = std::uint32_t(columns_d);
    job->grid.rows = std::uint32_t(rows_d);
    job->grid.geometry.horizontal_crs = HorizontalCrs::Wgs84Geographic;
    job->grid.geometry.vertical_datum = dataset == QStringLiteral("copernicus-glo30")
        ? VerticalDatum::Egm2008 : VerticalDatum::Unknown;
    job->grid.geometry.origin_x = request.west;
    job->grid.geometry.origin_y = request.north;
    job->grid.geometry.pixel_width = (request.east - request.west) / columns_d;
    job->grid.geometry.pixel_height = (request.north - request.south) / rows_d;
    job->options.fill_depressions = request.fill_depressions;
    job->options.breach_depressions = request.breach_depressions;
    job->options.maximum_breach_cut_depth_m = request.maximum_breach_cut_depth_m;
    job->options.maximum_breach_path_cells = request.maximum_breach_path_cells;
    job->options.maximum_breach_excavation_m3 = request.maximum_breach_excavation_m3;
    job->options.maximum_breach_candidates = request.maximum_breach_candidates;
    if (!QDir().mkpath(jobDirectory(id)))
    {
        QMutexLocker locker(&this->jobs_mutex);
        --this->terrain_admission_reservations;
        if (error != nullptr) *error = QStringLiteral("Cannot create job directory");
        return {};
    }
    if (!writeManifest(job))
    {
        // The job was never visible to other callers or workers.
        QDir(jobDirectory(id)).removeRecursively();
        QMutexLocker locker(&this->jobs_mutex);
        --this->terrain_admission_reservations;
        if (error != nullptr) *error = QStringLiteral("Cannot persist job metadata");
        return {};
    }
    job->queued_at = std::chrono::steady_clock::now();
    {
        QMutexLocker locker(&this->jobs_mutex);
        this->job_map.insert(id, job);
        this->terrain_queue.enqueue(job);
        --this->terrain_admission_reservations;
    }
    // Unlike QThreadPool::start for every accepted request, an explicit queue
    // bounds the number of pending closures and permits immediate removal of
    // canceled jobs even when all worker threads are busy.
    scheduleTerrainJobs();
    return id;
}

void AnalysisService::scheduleTerrainJobs()
{
    QMutexLocker locker(&this->jobs_mutex);
    const std::uint64_t cell_limit = this->config.max_active_terrain_cells == 0
        ? this->config.maximum_cells : this->config.max_active_terrain_cells;
    while (this->terrain_worker_slots < this->config.max_concurrent_analyses &&
           !this->terrain_queue.isEmpty())
    {
        const QSharedPointer<Job> next = this->terrain_queue.head();
        if (next->cancelled.load())
        {
            this->terrain_queue.dequeue();
            continue;
        }
        const std::uint64_t cells = std::uint64_t(next->grid.columns) * next->grid.rows;
        // FIFO: a large queued analysis cannot be bypassed indefinitely by
        // later small jobs. Wait for a running worker to release its cells.
        if (this->active_terrain_cells > cell_limit ||
            cells > cell_limit - this->active_terrain_cells) break;
        this->terrain_queue.dequeue();
        ++this->terrain_worker_slots;
        this->active_terrain_cells += cells;
        this->workers.start([this, next, cells]()
        {
            runJob(next);
            {
                QMutexLocker locker(&this->jobs_mutex);
                --this->terrain_worker_slots;
                this->active_terrain_cells -= cells;
            }
            scheduleTerrainJobs();
        });
    }
}

QJsonObject AnalysisService::job(const QString &id) const
{
    const QSharedPointer<Job> item = safeJobId(id) ? findJob(id) : QSharedPointer<Job>();
    return item ? jobJson(item) : QJsonObject();
}

QJsonArray AnalysisService::jobs() const
{
    QStringList ids;
    {
        QMutexLocker locker(&this->jobs_mutex);
        ids = this->job_map.keys();
    }
    ids.sort();
    QJsonArray response;
    for (const QString &id : ids)
    {
        const QSharedPointer<Job> item = findJob(id);
        if (item) response.append(jobJson(item));
    }
    return response;
}

QJsonObject AnalysisService::terrainJobCapacity() const
{
    QMutexLocker locker(&this->jobs_mutex);
    int queued = this->terrain_admission_reservations;
    int running = 0;
    for (const QSharedPointer<Job> &entry : std::as_const(this->job_map))
    {
        if (entry->state == QStringLiteral("queued")) ++queued;
        else if (entry->state != QStringLiteral("completed") &&
                 entry->state != QStringLiteral("failed") &&
                 entry->state != QStringLiteral("canceled")) ++running;
    }
    return QJsonObject{
        {QStringLiteral("queued"), queued},
        {QStringLiteral("running"), running},
        {QStringLiteral("outstanding"), queued + running},
        {QStringLiteral("limit"), this->config.max_outstanding_analyses},
        {QStringLiteral("active_cells"), double(this->active_terrain_cells)},
        {QStringLiteral("active_cell_limit"), double(this->config.max_active_terrain_cells == 0
            ? this->config.maximum_cells : this->config.max_active_terrain_cells)}
    };
}

bool AnalysisService::cancel(const QString &id)
{
    const QSharedPointer<Job> item = safeJobId(id) ? findJob(id) : QSharedPointer<Job>();
    if (!item) return false;
    bool persist = false;
    {
        QMutexLocker locker(&this->jobs_mutex);
        if (item->state == QStringLiteral("queued"))
        {
            // Remove any unscheduled job from the service's bounded queue.
            // If already dispatched, runJob() will skip the canceled job.
            item->cancelled.store(true);
            item->state = QStringLiteral("canceled");
            item->error = QStringLiteral("Job canceled");
            this->terrain_queue.removeAll(item);
            persist = true;
        }
        else if (item->state != QStringLiteral("completed") &&
                 item->state != QStringLiteral("failed") &&
                 item->state != QStringLiteral("canceled"))
            item->cancelled.store(true);
    }
    if (persist) writeManifest(item);
    return true;
}

QByteArray AnalysisService::layerMime(const QString &layer)
{
    return layerSpec(layer).mime;
}

QByteArray AnalysisService::rasterWindow(const QString &id, const QString &layer,
    int x, int y, int width, int height, QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    const Layer spec = layerSpec(layer);
    if (spec.bytes_per_cell == 0 || x < 0 || y < 0 || width < 1 || height < 1 ||
        width > 512 || height > 512 || std::int64_t(x) + width > item->grid.columns ||
        std::int64_t(y) + height > item->grid.rows)
    {
        if (error != nullptr) *error = QStringLiteral("Invalid raster layer or window");
        return {};
    }
    try
    {
        return readRectangle(QDir(jobDirectory(id)).filePath(spec.file),
                             item->grid.columns, x, y, width, height, spec.bytes_per_cell);
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

QByteArray AnalysisService::catchment(const QString &id, int x, int y, QString *error) const
{
    return catchmentImpl(id, x, y, nullptr, error);
}

QByteArray AnalysisService::catchmentImpl(const QString &id, int x, int y,
    const std::atomic<bool> *cancel_requested, QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    if (x < 0 || y < 0 || std::uint32_t(x) >= item->grid.columns || std::uint32_t(y) >= item->grid.rows)
    {
        if (error != nullptr) *error = QStringLiteral("Invalid catchment outlet cell");
        return {};
    }
    // Map the downstream file instead of materializing two whole-grid graph
    // arrays; still bound concurrent traversal and full-grid mask allocations.
    // Async workers may wait here; sync handlers return a busy error.
    if (cancel_requested)
    {
        while (!this->catchment_calculation_slot.tryAcquire(1, 50))
        {
            if (cancel_requested->load())
            {
                if (error) *error = QStringLiteral("Catchment task canceled");
                return {};
            }
        }
    }
    else if (!this->catchment_calculation_slot.tryAcquire())
    {
        if (error) *error = QStringLiteral("Catchment calculation busy");
        return {};
    }
    struct ReleaseSlot final
    {
        QSemaphore &slot;
        ~ReleaseSlot() { slot.release(); }
    } release {this->catchment_calculation_slot};
    try
    {
        const std::uint64_t cells = std::uint64_t(item->grid.columns) * item->grid.rows;
        const QString folder = jobDirectory(id);
        QFile links(QDir(folder).filePath(QStringLiteral("downstream.i64")));
        QFile counts(QDir(folder).filePath(QStringLiteral("upstream-count.u64")));
        if (!links.open(QIODevice::ReadOnly) || !counts.open(QIODevice::ReadOnly) ||
            links.size() != qint64(cells * 8) || counts.size() != qint64(cells * 8))
            throw std::runtime_error("Missing or incomplete flow graph");
        if (cancel_requested && cancel_requested->load())
            throw std::runtime_error("Catchment task canceled");
        // D8 contributors are always immediate neighbors. Map the immutable
        // downstream file and only read the eight bytes needed for outlet
        // validity from the count file. This avoids two extra 8-byte-per-cell
        // buffers and two reconstructed vectors on every catchment request.
        const std::uint64_t outlet = std::uint64_t(y) * item->grid.columns + std::uint64_t(x);
        if (!counts.seek(qint64(outlet * 8)))
            throw std::runtime_error("Cannot seek contributing-cell count");
        const QByteArray outlet_bytes = counts.read(8);
        if (outlet_bytes.size() != 8)
            throw std::runtime_error("Cannot read contributing-cell count");
        const uchar *mapped = links.map(0, qint64(cells * 8));
        if (!mapped)
            throw std::runtime_error("Cannot memory-map downstream raster");
        const std::vector<std::uint8_t> mask = delineateCatchmentWithReader(
            item->grid.columns, item->grid.rows, outlet,
            countValue(outlet_bytes.constData()) != 0,
            [mapped](std::uint64_t cell) {
                return downstreamValue(reinterpret_cast<const char *>(mapped + cell * 8));
            }, cancel_requested);
        return QByteArray(reinterpret_cast<const char *>(mask.data()), qsizetype(mask.size()));
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

QJsonObject AnalysisService::downstreamTrace(const QString &id, int x, int y,
                                               double maximum_distance_km,
                                               QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    if (x < 0 || y < 0 || std::uint32_t(x) >= item->grid.columns || std::uint32_t(y) >= item->grid.rows ||
        !std::isfinite(maximum_distance_km) || maximum_distance_km <= 0.0 ||
        maximum_distance_km > 1000.0)
    {
        if (error != nullptr) *error = QStringLiteral("Invalid trace origin or distance");
        return {};
    }
    try
    {
        const std::uint64_t cells = std::uint64_t(item->grid.columns) * item->grid.rows;
        const QDir folder(jobDirectory(id));
        QFile links(folder.filePath(QStringLiteral("downstream.i64")));
        QFile heights(folder.filePath(QStringLiteral("conditioned.f32")));
        if (!links.open(QIODevice::ReadOnly) || !heights.open(QIODevice::ReadOnly) ||
            links.size() != qint64(cells * 8) || heights.size() != qint64(cells * 4))
            throw std::runtime_error("Missing or incomplete stored trace layers");
        const FlowTrace path = traceDownstream(item->grid.geometry,
            item->grid.columns, item->grid.rows,
            std::uint64_t(y) * item->grid.columns + std::uint64_t(x),
            maximum_distance_km * 1000.0, 10000,
            [&links](std::uint64_t cell)
            {
                if (!links.seek(qint64(cell * 8)))
                    throw std::runtime_error("Cannot seek downstream raster");
                const QByteArray data = links.read(8);
                if (data.size() != 8)
                    throw std::runtime_error("Truncated downstream raster");
                return downstreamValue(data.constData());
            },
            [&heights](std::uint64_t cell)
            {
                if (!heights.seek(qint64(cell * 4)))
                    throw std::runtime_error("Cannot seek conditioned terrain raster");
                const QByteArray data = heights.read(4);
                if (data.size() != 4)
                    throw std::runtime_error("Truncated conditioned terrain raster");
                const quint32 bits = qFromLittleEndian<quint32>(data.constData());
                float height = 0.0f;
                std::memcpy(&height, &bits, sizeof(height));
                return height;
            });
        QString reason;
        switch (path.termination)
        {
            case TraceTermination::DomainOutlet: reason = QStringLiteral("domain_boundary"); break;
            case TraceTermination::InternalSink: reason = QStringLiteral("internal_sink"); break;
            case TraceTermination::DistanceLimit: reason = QStringLiteral("distance_limit"); break;
            case TraceTermination::PointLimit: reason = QStringLiteral("point_limit"); break;
        }
        QJsonArray coordinates;
        QJsonArray points;
        for (const TracePoint &point : path.points)
        {
            const MapCoordinate coordinate = pixelCentreWgs84(item->grid.geometry,
                std::uint32_t(point.cell_index % item->grid.columns),
                std::uint32_t(point.cell_index / item->grid.columns));
            coordinates.append(QJsonArray{coordinate.x, coordinate.y});
            points.append(QJsonObject{
                {QStringLiteral("cell_index"), double(point.cell_index)},
                {QStringLiteral("distance_m"), point.distance_from_start_m},
                {QStringLiteral("elevation_m"), double(point.conditioned_elevation_m)}
            });
        }
        return QJsonObject{
            {QStringLiteral("type"), QStringLiteral("Feature")},
            {QStringLiteral("geometry"), QJsonObject{
                {QStringLiteral("type"), path.points.size() == 1
                    ? QStringLiteral("Point") : QStringLiteral("LineString")},
                {QStringLiteral("coordinates"), path.points.size() == 1
                    ? QJsonValue(coordinates.first()) : QJsonValue(coordinates)}
            }},
            {QStringLiteral("properties"), QJsonObject{
                {QStringLiteral("termination"), reason},
                {QStringLiteral("traced_length_m"), path.traced_length_m},
                {QStringLiteral("net_slope"), path.net_slope},
                {QStringLiteral("points"), points},
                {QStringLiteral("domain_boundary_reached"),
                    path.termination == TraceTermination::DomainOutlet}
            }}
        };
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

QJsonObject AnalysisService::channelNetwork(const QString &id,
                                             double minimum_area_km2,
                                             QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    if (!std::isfinite(minimum_area_km2) || minimum_area_km2 <= 0.0 ||
        minimum_area_km2 > 1.0e9)
    {
        if (error != nullptr) *error = QStringLiteral("Invalid minimum contributing area");
        return {};
    }
    try
    {
        const HeavyQuerySlot slot(this->heavy_query_slots, nullptr);
        if (!slot.acquired()) throw std::runtime_error(HeavyQueryBusy.toStdString());
        const StoredFlowGraph graph(item->grid, jobDirectory(id));
        return channelNetworkJson(extractChannelNetwork(item->grid.geometry,
            item->grid.columns, item->grid.rows, graph.downstream(), graph.upstreamArea(),
            minimum_area_km2 * 1.0e6, 25000, 2000), item->grid);
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

QJsonObject AnalysisService::channelNetworkWindow(const QString &id,
    double minimum_area_km2, const DrainageWindow &window, QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    if (!std::isfinite(minimum_area_km2) || minimum_area_km2 <= 0.0 ||
        minimum_area_km2 > 1.0e9)
    {
        if (error != nullptr) *error = QStringLiteral("Invalid minimum contributing area");
        return {};
    }
    try
    {
        const HeavyQuerySlot slot(this->heavy_query_slots, nullptr);
        if (!slot.acquired()) throw std::runtime_error(HeavyQueryBusy.toStdString());
        const StoredFlowGraph graph(item->grid, jobDirectory(id));
        QJsonObject output = channelNetworkJson(extractWindowChannelNetwork(
            item->grid.geometry, item->grid.columns, item->grid.rows,
            graph.downstream(), graph.upstreamArea(), minimum_area_km2 * 1.0e6, window),
            item->grid);
        output.insert(QStringLiteral("window_clipped"), true);
        output.insert(QStringLiteral("local_stream_order"), true);
        return output;
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

QJsonObject AnalysisService::watershedBoundary(const QString &id, int x, int y,
                                                QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    if (x < 0 || y < 0 || std::uint32_t(x) >= item->grid.columns || std::uint32_t(y) >= item->grid.rows)
    {
        if (error != nullptr) *error = QStringLiteral("Invalid watershed outlet");
        return {};
    }
    const QByteArray mask_bytes = catchment(id, x, y, error);
    if (error != nullptr && !error->isEmpty()) return {};
    try
    {
        const std::vector<std::uint8_t> mask(mask_bytes.cbegin(), mask_bytes.cend());
        const WatershedBoundary boundary = polygonizeCatchment(item->grid, mask, 100000);
        QJsonArray polygons;
        for (const WatershedPolygon &polygon : boundary.polygons)
        {
            QJsonArray rings;
            for (const std::vector<GridVertex> &ring : polygon.rings)
            {
                QJsonArray coordinates;
                for (const GridVertex &vertex : ring)
                {
                    const MapCoordinate point = gridVertexWgs84(item->grid.geometry, vertex);
                    coordinates.append(QJsonArray{point.x, point.y});
                }
                rings.append(coordinates);
            }
            polygons.append(rings);
        }
        return QJsonObject{
            {QStringLiteral("type"), QStringLiteral("Feature")},
            {QStringLiteral("geometry"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("MultiPolygon")},
                {QStringLiteral("coordinates"), polygons}}},
            {QStringLiteral("properties"), QJsonObject{
                {QStringLiteral("outlet_cell"), double(std::uint64_t(y) * item->grid.columns + x)},
                {QStringLiteral("contributing_cells"), double(boundary.contributing_cells)},
                {QStringLiteral("contributing_area_m2"), boundary.contributing_area_m2},
                {QStringLiteral("touches_domain_boundary"), boundary.touches_domain_boundary},
                {QStringLiteral("boundary_truncation_possible"), boundary.touches_domain_boundary}}}
        };
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

QJsonObject AnalysisService::affectedNetwork(const QString &id,
                                              const QJsonObject &request,
                                              QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    const QJsonValue threshold_value = request.value(QStringLiteral("min_upstream_area_km2"));
    if (!threshold_value.isDouble() || !std::isfinite(threshold_value.toDouble()) ||
        threshold_value.toDouble() <= 0.0 || threshold_value.toDouble() > 1.0e9)
    {
        if (error != nullptr) *error = QStringLiteral("Invalid minimum contributing area");
        return {};
    }
    try
    {
        const std::vector<GeographicPolygon> polygons = parseGeoJsonPolygons(request);
        DrainageWindow window;
        if (request.contains(QStringLiteral("window")))
        {
            const QJsonValue value = request.value(QStringLiteral("window"));
            if (!value.isObject())
                throw std::invalid_argument("Invalid WGS84 spatial window");
            const QJsonObject input = value.toObject();
            for (const QString &name : {QStringLiteral("west"), QStringLiteral("east"),
                                        QStringLiteral("south"), QStringLiteral("north")})
                if (!input.value(name).isDouble())
                    throw std::invalid_argument("Invalid WGS84 spatial window coordinate");
            window.west = input.value(QStringLiteral("west")).toDouble();
            window.east = input.value(QStringLiteral("east")).toDouble();
            window.south = input.value(QStringLiteral("south")).toDouble();
            window.north = input.value(QStringLiteral("north")).toDouble();
        }
        const HeavyQuerySlot slot(this->heavy_query_slots, nullptr);
        if (!slot.acquired()) throw std::runtime_error(HeavyQueryBusy.toStdString());
        const StoredFlowGraph graph(item->grid, jobDirectory(id));
        const PolygonDrainageResult affected = downstreamNetworkFromPolygon(
            item->grid.geometry, item->grid.columns, item->grid.rows,
            graph.downstream(), graph.upstreamArea(), polygons,
            threshold_value.toDouble() * 1.0e6, window);
        QJsonObject output = channelNetworkJson(affected.network, item->grid);
        output.insert(QStringLiteral("source_cells"), double(affected.source_cells));
        output.insert(QStringLiteral("downstream_affected_cells"),
                      double(affected.downstream_affected_cells));
        output.insert(QStringLiteral("affected_domain_boundary_cells"),
                      double(affected.affected_boundary_cells));
        output.insert(QStringLiteral("window_clipped"), affected.clipped_to_window);
        output.insert(QStringLiteral("local_stream_order"), true);
        output.insert(QStringLiteral("model"), QStringLiteral("topological_d8_influence"));
        return output;
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}


QJsonObject AnalysisService::basins(const QString &id, int offset, int limit, QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    if (offset < 0 || limit < 1 || limit > 100)
    {
        if (error != nullptr) *error = QStringLiteral("Invalid basin pagination (limit 1..100)");
        return {};
    }
    try
    {
        const std::shared_ptr<const CachedBasinHierarchy> cached = cachedHierarchy(item);
        const DepressionHierarchy &hierarchy = cached->hierarchy;
        QJsonArray page;
        for (std::size_t index = std::size_t(offset);
             index < hierarchy.basins.size() && page.size() < limit; ++index)
            page.append(basinJson(hierarchy, id, hierarchy.basins[index]));
        return QJsonObject{
            {QStringLiteral("job_id"), id},
            {QStringLiteral("total_basins"), double(hierarchy.basins.size())},
            {QStringLiteral("offset"), offset},
            {QStringLiteral("limit"), limit},
            {QStringLiteral("has_more"), std::uint64_t(offset) + std::uint64_t(page.size()) < hierarchy.basins.size()},
            {QStringLiteral("basins"), page},
            {QStringLiteral("model"), QStringLiteral("static_topographic_storage")}
        };
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

QJsonObject AnalysisService::basin(const QString &id, const QString &basin_uuid,
                                  QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    try
    {
        const std::shared_ptr<const CachedBasinHierarchy> cached = cachedHierarchy(item);
        return basinJson(cached->hierarchy, id, findBasin(*cached, basin_uuid));
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

QJsonObject AnalysisService::basinStorage(const QString &id, const QString &basin_uuid,
                                          double level_m, QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    if (!std::isfinite(level_m))
    {
        if (error != nullptr) *error = QStringLiteral("Invalid basin water level");
        return {};
    }
    try
    {
        const QString folder = jobDirectory(id);
        const std::shared_ptr<const CachedBasinHierarchy> cached = cachedHierarchy(item);
        const DepressionHierarchy &hierarchy = cached->hierarchy;
        const DepressionBasin &selected = findBasin(*cached, basin_uuid);
        // Only the basin's cells and their neighbours are read from the
        // mapped DEM; the visited set is one bit per cell. Cheap enough not
        // to need a heavy-query slot.
        const MappedRaster dem(QDir(folder).filePath(QStringLiteral("elevation.f32")),
            std::uint64_t(item->grid.columns) * item->grid.rows * 4,
            "Missing or incomplete source elevation raster");
        const std::function<float(std::uint64_t)> elevation = [base = dem.bytes()](std::uint64_t cell)
        {
            const quint32 bits = qFromLittleEndian<quint32>(base + cell * 4);
            float height = 0.0f;
            std::memcpy(&height, &bits, sizeof(height));
            return height;
        };
        const BasinStorageState storage = basinStorageAtLevel(item->grid.geometry,
            item->grid.columns, item->grid.rows, elevation, hierarchy, selected.index, level_m);
        QJsonObject output = stateJson(storage);
        output.insert(QStringLiteral("basin_id"), basinUuid(id, selected.index));
        output.insert(QStringLiteral("job_id"), id);
        output.insert(QStringLiteral("model"), QStringLiteral("static_topographic_storage"));
        return output;
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

QJsonObject AnalysisService::basinStorageCurve(const QString &id, const QString &basin_uuid,
                                               int samples, QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    if (samples < 2 || samples > 16)
    {
        if (error != nullptr) *error = QStringLiteral("Invalid basin curve sample count (2..16)");
        return {};
    }
    try
    {
        const QString folder = jobDirectory(id);
        const std::shared_ptr<const CachedBasinHierarchy> cached = cachedHierarchy(item);
        const DepressionHierarchy &hierarchy = cached->hierarchy;
        const DepressionBasin &selected = findBasin(*cached, basin_uuid);
        if (!std::isfinite(selected.spill_elevation_m))
            throw std::invalid_argument("Basin has no known spill elevation");
        // Only the basin's cells and their neighbours are read from the
        // mapped DEM; the visited set is one bit per cell. Cheap enough not
        // to need a heavy-query slot.
        const MappedRaster dem(QDir(folder).filePath(QStringLiteral("elevation.f32")),
            std::uint64_t(item->grid.columns) * item->grid.rows * 4,
            "Missing or incomplete source elevation raster");
        const std::function<float(std::uint64_t)> elevation = [base = dem.bytes()](std::uint64_t cell)
        {
            const quint32 bits = qFromLittleEndian<quint32>(base + cell * 4);
            float height = 0.0f;
            std::memcpy(&height, &bits, sizeof(height));
            return height;
        };
        QJsonArray curve;
        for (int index = 0; index < samples; ++index)
        {
            const double level = index + 1 == samples ? selected.spill_elevation_m :
                selected.formation_elevation_m +
                (selected.spill_elevation_m - selected.formation_elevation_m) *
                (double(index) / double(samples - 1));
            const BasinStorageState storage = basinStorageAtLevel(item->grid.geometry,
                item->grid.columns, item->grid.rows, elevation, hierarchy,
                selected.index, level, 2000000);
            curve.append(stateJson(storage));
        }
        return QJsonObject{
            {QStringLiteral("job_id"), id},
            {QStringLiteral("basin_id"), basinUuid(id, selected.index)},
            {QStringLiteral("model"), QStringLiteral("static_topographic_storage")},
            {QStringLiteral("samples"), curve}
        };
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

} // namespace Aowis::Hydrology

namespace Aowis::Hydrology
{
namespace
{
// Scenario manifests are the commit markers. Orphaned raster files left by
// interrupted writes are ignored by list and detail operations.
QJsonObject loadRainfallManifest(const QString &folder, const QString &uuid)
{
    const QUuid parsed(QStringLiteral("{") + uuid + QStringLiteral("}"));
    if (parsed.isNull() || parsed.toString(QUuid::WithoutBraces) != uuid)
        throw std::invalid_argument("Invalid rainfall scenario UUID");
    QFile file(QDir(folder).filePath(uuid + QStringLiteral("/scenario.json")));
    if (!file.open(QIODevice::ReadOnly))
        throw std::invalid_argument("Unknown rainfall scenario UUID");
    if (file.size() > Internal::MaximumScenarioManifestBytes)
        throw std::runtime_error("Corrupt rainfall scenario manifest: exceeds 1 MiB");
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject object = document.object();
    if (!document.isObject() || object.value(QStringLiteral("id")) != uuid)
        throw std::runtime_error("Corrupt rainfall scenario manifest");
    return object;
}
}

QJsonObject AnalysisService::createRainfallScenario(
    const QString &id, const QJsonObject &request, QString *error) const
{
    return createRainfallScenarioImpl(id, request, nullptr, error);
}

QJsonObject AnalysisService::createRainfallScenarioImpl(
    const QString &id, const QJsonObject &request,
    const std::atomic<bool> *cancel_requested, QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    try
    {
        for (const QString &name : {QStringLiteral("depth_mm"),
                                     QStringLiteral("duration_seconds"),
                                     QStringLiteral("effective_runoff_fraction")})
            if (!request.value(name).isDouble())
                throw std::invalid_argument("Invalid rainfall scenario numeric field");
        const QString name = request.value(QStringLiteral("name")).toString();
        if (name.size() > 128 || name.contains(QChar('\n')) || name.contains(QChar('\r')) ||
            (!request.value(QStringLiteral("name")).isUndefined() &&
             !request.value(QStringLiteral("name")).isString()))
            throw std::invalid_argument("Invalid rainfall scenario name");
        RainfallScenario scenario;
        scenario.polygons = parseGeoJsonPolygons(request);
        scenario.depth_mm = request.value(QStringLiteral("depth_mm")).toDouble();
        scenario.duration_seconds = request.value(QStringLiteral("duration_seconds")).toDouble();
        scenario.effective_runoff_fraction = request.value(
            QStringLiteral("effective_runoff_fraction")).toDouble();
        const QString scenarios_folder = QDir(jobDirectory(id)).filePath(
            QStringLiteral("rainfall-scenarios"));
        // A scenario becomes committed only after scenario.json is published.
        // Counting directories here is incorrect: an in-flight creation already
        // owns a reservation and may have created its UUID directory. Count
        // committed manifests separately, and atomically transfer a reservation
        // into that count when the manifest is committed.
        {
            QMutexLocker locker(&this->rainfall_creation_mutex);
            if (!this->rainfall_committed_scenario_counts.contains(id))
            {
                int committed = 0;
                const QDir existing(scenarios_folder);
                for (const QString &uuid : existing.entryList(
                         QDir::Dirs | QDir::NoDotAndDotDot))
                    if (QFileInfo::exists(existing.filePath(
                            uuid + QStringLiteral("/scenario.json"))))
                        ++committed;
                this->rainfall_committed_scenario_counts.insert(id, committed);
            }
            if (this->rainfall_committed_scenario_counts.value(id) +
                this->rainfall_creation_reservations.value(id) >= 64)
                throw std::invalid_argument("Too many rainfall scenarios for one terrain job");
            ++this->rainfall_creation_reservations[id];
        }
        bool reservation_active = true;
        const auto release_reservation = [this, id, &reservation_active](
                                             const AnalysisService *)
        {
            if (!reservation_active) return;
            QMutexLocker locker(&this->rainfall_creation_mutex);
            if (--this->rainfall_creation_reservations[id] == 0)
                this->rainfall_creation_reservations.remove(id);
        };
        const std::unique_ptr<const AnalysisService, decltype(release_reservation)> reservation(
            this, release_reservation);
        // Whole-DEM load plus two full-grid result rasters: admitted through
        // the shared heavy-query slots (synchronous callers fail fast,
        // background tasks wait cancellably).
        const HeavyQuerySlot slot(this->heavy_query_slots, cancel_requested);
        if (!slot.acquired())
            throw std::runtime_error(cancel_requested != nullptr
                ? "Rainfall scenario canceled" : HeavyQueryBusy.toStdString());
        const ElevationGrid source = loadOriginalElevation(
            jobDirectory(id), item->grid, cancel_requested);
        const RainfallRunoff result = calculateRainfallRunoff(source, scenario,
            500000, cancel_requested);
        if (cancel_requested != nullptr && cancel_requested->load())
            throw std::runtime_error("Rainfall scenario canceled");
        if (result.contains_missing_dem_coverage)
            throw std::invalid_argument("Rainfall polygons intersect cells without elevation data");
        if (result.wet_cells == 0)
            throw std::invalid_argument("Rainfall polygon covers no DEM cells");
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString folder = QDir(scenarios_folder).filePath(uuid);
        if (!QDir().mkpath(folder))
            throw std::runtime_error("Cannot create rainfall scenario directory");
        try
        {
            const QString runoff_hash = writeArray(
                QDir(folder).filePath(QStringLiteral("runoff.f64")), result.runoff_volume_m3);
            const QString coverage_hash = writeArray(
                QDir(folder).filePath(QStringLiteral("coverage.f32")), result.covered_fraction);
            if (cancel_requested != nullptr && cancel_requested->load())
                throw std::runtime_error("Rainfall scenario canceled");
            const QJsonObject summary{
                {QStringLiteral("id"), uuid},
                {QStringLiteral("job_id"), id},
                {QStringLiteral("name"), name},
                {QStringLiteral("model"), QStringLiteral("static_uniform_rainfall_runoff")},
                {QStringLiteral("depth_mm"), scenario.depth_mm},
                {QStringLiteral("duration_seconds"), scenario.duration_seconds},
                {QStringLiteral("effective_runoff_fraction"), scenario.effective_runoff_fraction},
                {QStringLiteral("geometry"), request.value(QStringLiteral("geometry")).isObject()
                    ? request.value(QStringLiteral("geometry")) : QJsonValue(request)},
                {QStringLiteral("wet_cells"), double(result.wet_cells)},
                {QStringLiteral("covered_area_m2"), result.covered_area_m2},
                {QStringLiteral("gross_rainfall_volume_m3"), result.gross_rainfall_volume_m3},
                {QStringLiteral("effective_runoff_volume_m3"), result.effective_runoff_volume_m3},
                {QStringLiteral("retained_or_lost_volume_m3"), result.retained_or_lost_volume_m3},
                {QStringLiteral("average_effective_runoff_m3_s"), result.average_effective_runoff_m3_s},
                {QStringLiteral("touches_domain_boundary"), result.touches_domain_boundary},
                {QStringLiteral("runoff_raster_format"), QStringLiteral("f64le_m3_per_cell")},
                {QStringLiteral("coverage_raster_format"), QStringLiteral("f32le_fraction")},
                {QStringLiteral("artifact_integrity"), QJsonObject{
                    {QStringLiteral("runoff.f64"), artifactRecord(
                        std::uint64_t(result.runoff_volume_m3.size()) * 8, runoff_hash)},
                    {QStringLiteral("coverage.f32"), artifactRecord(
                        std::uint64_t(result.covered_fraction.size()) * 4, coverage_hash)}
                }}
            };
            if (cancel_requested != nullptr && cancel_requested->load())
                throw std::runtime_error("Rainfall scenario canceled");
            QSaveFile file(QDir(folder).filePath(QStringLiteral("scenario.json")));
            const QByteArray json_bytes = QJsonDocument(summary).toJson(QJsonDocument::Compact);
            // Never commit a manifest that loadRainfallManifest() would refuse:
            // it would consume quota yet remain unreadable.
            if (json_bytes.size() > Internal::MaximumScenarioManifestBytes)
                throw std::invalid_argument("Invalid rainfall scenario: manifest exceeds 1 MiB");
            if (!file.open(QIODevice::WriteOnly) ||
                file.write(json_bytes) != json_bytes.size() || !file.commit())
                throw std::runtime_error("Cannot persist rainfall scenario manifest");
            // Publication and quota accounting form a single logical commit:
            // a concurrent submitter sees either the reservation or the
            // committed result, never both (nor neither).
            {
                QMutexLocker locker(&this->rainfall_creation_mutex);
                ++this->rainfall_committed_scenario_counts[id];
                if (--this->rainfall_creation_reservations[id] == 0)
                    this->rainfall_creation_reservations.remove(id);
                reservation_active = false;
            }
            return summary;
        }
        catch (...)
        {
            QDir(folder).removeRecursively();
            throw;
        }
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

QJsonObject AnalysisService::rainfallTaskJson(const QSharedPointer<RainfallTask> &task) const
{
    // This method is called with rainfall_tasks_mutex held.
    QJsonObject result{
        {QStringLiteral("task_id"), task->id},
        {QStringLiteral("job_id"), task->analysis_id},
        {QStringLiteral("state"), task->state}
    };
    if (!task->error.isEmpty()) result.insert(QStringLiteral("error"), task->error);
    if (task->state == QStringLiteral("completed"))
        result.insert(QStringLiteral("scenario"), task->result);
    return result;
}

QJsonObject AnalysisService::startRainfallScenario(
    const QString &id, const QJsonObject &request, QString *error)
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> job_item = completedJob(id, error);
    if (!job_item) return {};
    // Reject structurally invalid input at submission time. All expensive
    // raster work and geometric intersection stays on the worker thread.
    try
    {
        for (const QString &field : {QStringLiteral("depth_mm"),
                                     QStringLiteral("duration_seconds"),
                                     QStringLiteral("effective_runoff_fraction")})
            if (!request.value(field).isDouble() ||
                !std::isfinite(request.value(field).toDouble()))
                throw std::invalid_argument("Invalid rainfall scenario numeric field");
        const QJsonValue name = request.value(QStringLiteral("name"));
        if ((!name.isUndefined() && !name.isString()) ||
            name.toString().size() > 128 || name.toString().contains(QChar('\n')) ||
            name.toString().contains(QChar('\r')))
            throw std::invalid_argument("Invalid rainfall scenario name");
        const double depth = request.value(QStringLiteral("depth_mm")).toDouble();
        const double duration = request.value(QStringLiteral("duration_seconds")).toDouble();
        const double fraction = request.value(QStringLiteral("effective_runoff_fraction")).toDouble();
        if (depth < 0.0 || depth > 100000.0 || duration <= 0.0 ||
            duration > 31557600.0 || fraction < 0.0 || fraction > 1.0)
            throw std::invalid_argument("Invalid rainfall scenario or DEM grid");
        const auto polygons = parseGeoJsonPolygons(request);
        if (polygons.empty() || polygons.size() > 32)
            throw std::invalid_argument("Invalid rainfall scenario or DEM grid");
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
    QSharedPointer<RainfallTask> task = QSharedPointer<RainfallTask>::create();
    task->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    task->analysis_id = id;
    task->request = request;
    QJsonObject output;
    {
        QMutexLocker locker(&this->rainfall_tasks_mutex);
        int outstanding = 0;
        for (const QSharedPointer<RainfallTask> &entry : std::as_const(this->rainfall_tasks))
            if (entry->state == QStringLiteral("queued") ||
                entry->state == QStringLiteral("running") ||
                entry->state == QStringLiteral("cancel_requested")) ++outstanding;
        if (outstanding >= 8)
        {
            if (error != nullptr) *error = QStringLiteral("Rainfall scenario queue full (8 active/queued tasks)");
            return {};
        }
        // Keep metadata finite, without invalidating active work. Old terminal
        // tasks are transient; persisted scenario results are unaffected.
        while (this->rainfall_task_order.size() >= 256)
        {
            bool pruned = false;
            for (qsizetype index = 0; index < this->rainfall_task_order.size(); ++index)
            {
                const QString oldest = this->rainfall_task_order.at(index);
                const QSharedPointer<RainfallTask> entry = this->rainfall_tasks.value(oldest);
                if (entry && (entry->state == QStringLiteral("queued") ||
                              entry->state == QStringLiteral("running") ||
                              entry->state == QStringLiteral("cancel_requested"))) continue;
                this->rainfall_task_order.removeAt(index);
                this->rainfall_tasks.remove(oldest);
                pruned = true;
                break;
            }
            if (!pruned) break; // At most eight active tasks exist.
        }
        this->rainfall_tasks.insert(task->id, task);
        this->rainfall_task_order.enqueue(task->id);
        this->rainfall_queue.enqueue(task);
        output = rainfallTaskJson(task);
    }
    scheduleRainfallTasks();
    return output;
}

void AnalysisService::scheduleRainfallTasks()
{
    QMutexLocker locker(&this->rainfall_tasks_mutex);
    while (this->rainfall_worker_slots < 2 && !this->rainfall_queue.isEmpty())
    {
        const QSharedPointer<RainfallTask> task = this->rainfall_queue.dequeue();
        if (task->cancelled.load()) continue;
        ++this->rainfall_worker_slots;
        this->rainfall_workers.start([this, task]()
        {
            runRainfallTask(task);
            {
                QMutexLocker lock(&this->rainfall_tasks_mutex);
                --this->rainfall_worker_slots;
            }
            scheduleRainfallTasks();
        });
    }
}

void AnalysisService::runRainfallTask(const QSharedPointer<RainfallTask> &task)
{
    {
        QMutexLocker locker(&this->rainfall_tasks_mutex);
        if (task->cancelled.load()) return;
        task->state = QStringLiteral("running");
    }
    QString error;
    const QJsonObject result = createRainfallScenarioImpl(
        task->analysis_id, task->request, &task->cancelled, &error);
    {
        QMutexLocker locker(&this->rainfall_tasks_mutex);
        if (!result.isEmpty())
        {
            // Commit wins the race with a late cancellation request.
            task->state = QStringLiteral("completed");
            task->result = result;
        }
        else if (task->cancelled.load())
            task->state = QStringLiteral("canceled");
        else
        {
            task->state = QStringLiteral("failed");
            task->error = error.isEmpty() ? QStringLiteral("Rainfall scenario failed") : error;
        }
        task->request = {}; // no longer hold client geometry after completion
    }
}

QJsonObject AnalysisService::rainfallScenarioTask(
    const QString &id, const QString &task_id, QString *error) const
{
    if (error != nullptr) error->clear();
    if (!safeJobId(id) || !safeJobId(task_id))
    {
        if (error != nullptr) *error = QStringLiteral("Unknown rainfall scenario task");
        return {};
    }
    QMutexLocker locker(&this->rainfall_tasks_mutex);
    const QSharedPointer<RainfallTask> task = this->rainfall_tasks.value(task_id);
    if (!task || task->analysis_id != id)
    {
        if (error != nullptr) *error = QStringLiteral("Unknown rainfall scenario task");
        return {};
    }
    return rainfallTaskJson(task);
}

bool AnalysisService::cancelRainfallScenarioTask(
    const QString &id, const QString &task_id, QString *error)
{
    if (error != nullptr) error->clear();
    QMutexLocker locker(&this->rainfall_tasks_mutex);
    const QSharedPointer<RainfallTask> task = safeJobId(id) && safeJobId(task_id)
        ? this->rainfall_tasks.value(task_id) : QSharedPointer<RainfallTask>();
    if (!task || task->analysis_id != id)
    {
        if (error != nullptr) *error = QStringLiteral("Unknown rainfall scenario task");
        return false;
    }
    if (task->state == QStringLiteral("queued"))
    {
        task->cancelled.store(true);
        task->state = QStringLiteral("canceled");
        task->request = {};
        this->rainfall_queue.removeAll(task);
    }
    else if (task->state == QStringLiteral("running"))
    {
        task->cancelled.store(true);
        task->state = QStringLiteral("cancel_requested");
    }
    return true;
}

QJsonArray AnalysisService::rainfallScenarios(const QString &id, QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    try
    {
        QJsonArray output;
        const QString folder = QDir(jobDirectory(id)).filePath(
            QStringLiteral("rainfall-scenarios"));
        const QStringList identifiers = QDir(folder).entryList(
            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &uuid : identifiers)
        {
            if (!QFileInfo::exists(QDir(folder).filePath(uuid + QStringLiteral("/scenario.json"))))
                continue;
            // One damaged manifest must not hide every other scenario.
            try { output.append(loadRainfallManifest(folder, uuid)); }
            catch (const std::exception &ex)
            {
                qWarning("Skipping unreadable rainfall scenario %s/%s: %s",
                         qPrintable(id), qPrintable(uuid), ex.what());
            }
        }
        return output;
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

QJsonObject AnalysisService::rainfallScenario(
    const QString &id, const QString &scenario_uuid, QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    try
    {
        return loadRainfallManifest(QDir(jobDirectory(id)).filePath(
            QStringLiteral("rainfall-scenarios")), scenario_uuid);
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

QByteArray AnalysisService::rainfallRunoffWindow(
    const QString &id, const QString &scenario_uuid,
    int x, int y, int width, int height, QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
        width > 4096 || height > 4096 || std::uint64_t(width) * height > 1000000 ||
        std::uint64_t(x) + width > item->grid.columns ||
        std::uint64_t(y) + height > item->grid.rows)
    {
        if (error != nullptr) *error = QStringLiteral("Invalid rainfall raster window");
        return {};
    }
    try
    {
        const QString folder = QDir(jobDirectory(id)).filePath(
            QStringLiteral("rainfall-scenarios"));
        (void)loadRainfallManifest(folder, scenario_uuid);
        const QString path = QDir(folder).filePath(
            scenario_uuid + QStringLiteral("/runoff.f64"));
        if (QFileInfo(path).size() != qint64(std::uint64_t(item->grid.columns) * item->grid.rows * 8))
            throw std::runtime_error("Incomplete rainfall scenario runoff raster");
        return readRectangle(path, item->grid.columns, x, y, width, height, 8);
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}
} // namespace Aowis::Hydrology

namespace Aowis::Hydrology
{
namespace
{
// Fingerprints describe the bytes of committed immutable inputs, not their
// timestamps. Persisted SHA-256 IDs are recorded when producing terrain and
// rainfall artifacts. For old manifests without IDs we calculate them on
// demand. This also checks published IDs against the files used by a task.
//
// A process-local stamp memo avoids rereading multi-gigabyte rasters for
// every status/page request. The stamp includes inode and nanosecond ctime on
// Unix, so an ordinary overwrite that restores size + mtime is detected.
// External in-place mutation is unsupported; the memo is not a security
// boundary against hostile filesystem manipulation or concurrent writers.
struct SourceStamp
{
    qint64 bytes = -1;
    qint64 mtime_seconds = 0;
    qint64 mtime_nanoseconds = 0;
    qint64 change_seconds = 0;
    qint64 change_nanoseconds = 0;
    quint64 device = 0;
    quint64 inode = 0;
    bool operator==(const SourceStamp &) const = default;
};

SourceStamp sourceStamp(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink())
        throw std::runtime_error("Missing immutable redistribution source file");
    SourceStamp stamp;
#if defined(Q_OS_UNIX)
    struct stat stat_result {};
    if (::stat(QFile::encodeName(path).constData(), &stat_result) != 0 ||
        !S_ISREG(stat_result.st_mode))
        throw std::runtime_error("Cannot stat immutable redistribution source file");
    stamp.bytes = stat_result.st_size;
    stamp.device = stat_result.st_dev;
    stamp.inode = stat_result.st_ino;
#if defined(Q_OS_DARWIN)
    stamp.mtime_seconds = stat_result.st_mtimespec.tv_sec;
    stamp.mtime_nanoseconds = stat_result.st_mtimespec.tv_nsec;
    stamp.change_seconds = stat_result.st_ctimespec.tv_sec;
    stamp.change_nanoseconds = stat_result.st_ctimespec.tv_nsec;
#else
    stamp.mtime_seconds = stat_result.st_mtim.tv_sec;
    stamp.mtime_nanoseconds = stat_result.st_mtim.tv_nsec;
    stamp.change_seconds = stat_result.st_ctim.tv_sec;
    stamp.change_nanoseconds = stat_result.st_ctim.tv_nsec;
#endif
#else
    stamp.bytes = info.size();
    stamp.mtime_seconds = info.lastModified().toMSecsSinceEpoch();
    // This platform has only the Qt timestamp precision available.
    stamp.change_seconds = info.metadataChangeTime().toMSecsSinceEpoch();
#endif
    if (stamp.bytes < 0)
        throw std::runtime_error("Invalid immutable redistribution source length");
    return stamp;
}

QString sourceDigest(const QString &path, const QString &expected = QString(),
                     qint64 expected_bytes = -1)
{
    struct Entry { SourceStamp stamp; QString sha256; };
    static QMutex digest_mutex;
    static QHash<QString, Entry> digests;
    const QString key = QFileInfo(path).absoluteFilePath();
    const SourceStamp stamp = sourceStamp(path);
    if (expected_bytes >= 0 && stamp.bytes != expected_bytes)
        throw std::runtime_error("Immutable redistribution source length mismatch");
    QString digest;
    {
        QMutexLocker lock(&digest_mutex);
        const auto entry = digests.constFind(key);
        if (entry != digests.cend() && entry->stamp == stamp)
            digest = entry->sha256;
    }
    if (digest.isEmpty())
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error("Cannot read immutable redistribution source file");
        QCryptographicHash hash(QCryptographicHash::Sha256);
        while (!file.atEnd())
        {
            const QByteArray block = file.read(1024 * 1024);
            if (block.isEmpty())
                throw std::runtime_error("Cannot hash immutable redistribution source file");
            hash.addData(block);
        }
        if (sourceStamp(path) != stamp)
            throw std::runtime_error("Immutable redistribution source changed while hashing");
        digest = QString::fromLatin1(hash.result().toHex());
        QMutexLocker lock(&digest_mutex);
        // This is only a performance memo, not persistent job state.
        // Keep it bounded even when the service handles many scenarios.
        if (digests.size() >= 4096)
            digests.clear();
        digests.insert(key, Entry{stamp, digest});
    }
    if (!expected.isEmpty() && digest != expected)
        throw std::runtime_error("Immutable redistribution source SHA-256 mismatch");
    return digest;
}

QString recordedSourceDigest(const QString &path, const QJsonObject &integrity,
                             const QString &name)
{
    // A missing inventory is allowed for persisted pre-integrity analyses.
    if (integrity.isEmpty())
        return sourceDigest(path);
    const QJsonObject record = integrity.value(name).toObject();
    const QString digest = record.value(QStringLiteral("sha256")).toString();
    const qint64 bytes = qint64(record.value(QStringLiteral("size_bytes")).toDouble(-1));
    if (digest.size() != 64 || bytes < 0 ||
        !std::all_of(digest.cbegin(), digest.cend(), [](QChar c) {
            return (c >= QChar('0') && c <= QChar('9')) ||
                   (c >= QChar('a') && c <= QChar('f'));
        }))
        throw std::runtime_error("Invalid immutable redistribution source identity");
    return sourceDigest(path, digest, bytes);
}

RedistributionArchiveIdentity redistributionIdentity(
    const QString &job_id, const QString &scenario_id,
    const QString &job_folder, const QString &scenario_folder,
    std::uint32_t columns, std::uint32_t rows)
{
    RedistributionArchiveIdentity identity;
    identity.columns = columns;
    identity.rows = rows;
    const QUuid job_uuid(QStringLiteral("{") + job_id + QStringLiteral("}"));
    const QUuid scenario_uuid(QStringLiteral("{") + scenario_id + QStringLiteral("}"));
    if (job_uuid.isNull() || scenario_uuid.isNull())
        throw std::invalid_argument("Invalid cache job or scenario UUID");
    const QByteArray job_bytes = job_uuid.toRfc4122();
    const QByteArray scenario_bytes = scenario_uuid.toRfc4122();
    for (std::size_t index = 0; index < 16; ++index)
    {
        identity.job_uuid[index] = std::uint8_t(job_bytes.at(qsizetype(index)));
        identity.scenario_uuid[index] = std::uint8_t(scenario_bytes.at(qsizetype(index)));
    }
    QFile job_file(QDir(job_folder).filePath(QStringLiteral("job.json")));
    if (!job_file.open(QIODevice::ReadOnly) || job_file.size() > 1024 * 1024)
        throw std::runtime_error("Cannot read completed terrain source manifest");
    const QJsonObject job = QJsonDocument::fromJson(job_file.readAll()).object();
    if (job.value(QStringLiteral("id")).toString() != job_id ||
        job.value(QStringLiteral("state")).toString() != QStringLiteral("completed"))
        throw std::runtime_error("Invalid completed terrain source manifest");
    const QString scenario_root = QFileInfo(scenario_folder).absolutePath();
    const QJsonObject scenario = loadRainfallManifest(scenario_root, scenario_id);
    const QJsonObject job_integrity = job.value(QStringLiteral("artifact_integrity")).toObject();
    const QJsonObject scenario_integrity = scenario.value(QStringLiteral("artifact_integrity")).toObject();
    if ((job.contains(QStringLiteral("artifact_integrity")) && job_integrity.isEmpty()) ||
        (scenario.contains(QStringLiteral("artifact_integrity")) &&
         scenario_integrity.size() != 2))
        throw std::runtime_error("Incomplete immutable redistribution source identity");

    // Domain separation and a model identity ensure old timestamp-based
    // archives are not accidentally reused after this change. Bump the
    // algorithm tag if the numerical redistribution semantics change.
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("aowis-redistribution-source-v2:static-volume-v1\n"));
    hash.addData(QByteArray::number(columns) + 'x' + QByteArray::number(rows) + '\n');
    const auto addInput = [&hash](const char *name, const QString &digest)
    {
        hash.addData(QByteArray(name) + ':' + digest.toLatin1() + '\n');
    };
    addInput("elevation.f32", recordedSourceDigest(
        QDir(job_folder).filePath(QStringLiteral("elevation.f32")),
        job_integrity, QStringLiteral("elevation.f32")));
    addInput("basins.aowbin", recordedSourceDigest(
        QDir(job_folder).filePath(QStringLiteral("basins.aowbin")),
        job_integrity, QStringLiteral("basins.aowbin")));
    addInput("runoff.f64", recordedSourceDigest(
        QDir(scenario_folder).filePath(QStringLiteral("runoff.f64")),
        scenario_integrity, QStringLiteral("runoff.f64")));
    // scenario.json is compact and self-referential hashes are avoided:
    // hashing its published bytes covers geometry, inputs and raster IDs.
    addInput("scenario.json", sourceDigest(
        QDir(scenario_folder).filePath(QStringLiteral("scenario.json"))));
    const QByteArray signature = hash.result();
    for (std::size_t index = 0; index < 16; ++index)
        identity.source_fingerprint[index] = std::uint8_t(signature.at(qsizetype(index)));
    return identity;
}

template <std::size_t Count>
std::array<std::uint8_t, Count> readCacheBytes(QFile &file)
{
    std::array<std::uint8_t, Count> bytes{};
    if (file.read(reinterpret_cast<char *>(bytes.data()), qsizetype(bytes.size())) !=
        qsizetype(bytes.size()))
        throw std::runtime_error("Truncated redistribution cache");
    return bytes;
}

struct CachedRedistributionPage
{
    RedistributionArchiveHeader header;
    std::vector<RedistributionArchiveRecord> records;
};

CachedRedistributionPage loadRedistributionPage(
    const QString &path, const RedistributionArchiveIdentity &expected,
    int offset, int limit)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Redistribution cache not available");
    CachedRedistributionPage page;
    page.header = decodeRedistributionHeader(
        readCacheBytes<RedistributionArchiveHeaderBytes>(file));
    const RedistributionArchiveIdentity &stored = page.header.identity;
    if (stored.job_uuid != expected.job_uuid ||
        stored.scenario_uuid != expected.scenario_uuid ||
        stored.source_fingerprint != expected.source_fingerprint ||
        stored.columns != expected.columns || stored.rows != expected.rows)
        throw std::runtime_error("Redistribution cache is stale");
    const std::uint64_t count = page.header.basin_count;
    if (count > std::uint64_t((std::numeric_limits<qint64>::max() -
            qint64(RedistributionArchiveHeaderBytes)) / RedistributionArchiveRecordBytes) ||
        file.size() != qint64(RedistributionArchiveHeaderBytes +
            count * RedistributionArchiveRecordBytes))
        throw std::runtime_error("Invalid redistribution cache length");
    const std::uint64_t start = std::min(std::uint64_t(offset), count);
    const std::uint64_t end = std::min(start + std::uint64_t(limit), count);
    if (!file.seek(qint64(RedistributionArchiveHeaderBytes +
                          start * RedistributionArchiveRecordBytes)))
        throw std::runtime_error("Cannot seek redistribution cache");
    page.records.reserve(std::size_t(end - start));
    for (std::uint64_t index = start; index < end; ++index)
        page.records.push_back(decodeRedistributionRecord(
            readCacheBytes<RedistributionArchiveRecordBytes>(file), index, count));
    return page;
}

// Writes a complete atomic result. There is no partially published cache if
// the server exits while computing or writing it.
void writeRedistributionCache(const QString &path,
                              const RedistributionArchiveIdentity &identity,
                              const RunoffRedistribution &result,
                              const DepressionHierarchy &hierarchy,
                              const std::atomic<bool> *cancel_requested)
{
    if (result.basins.size() != hierarchy.basins.size())
        throw std::runtime_error("Redistribution hierarchy count mismatch");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot create redistribution cache");
    const std::array<std::uint8_t, RedistributionArchiveHeaderBytes> header =
        encodeRedistributionHeader(identity, result);
    if (file.write(reinterpret_cast<const char *>(header.data()), qsizetype(header.size())) !=
        qsizetype(header.size()))
        throw std::runtime_error("Cannot write redistribution cache header");
    QByteArray records;
    records.reserve(65536);
    for (std::size_t index = 0; index < result.basins.size(); ++index)
    {
        if ((index & 4095U) == 0 && cancel_requested != nullptr &&
            cancel_requested->load())
            throw std::runtime_error("Runoff redistribution cancelled");
        RedistributionArchiveRecord row;
        row.state = result.basins[index];
        row.parent_index = hierarchy.basins[index].parent_index;
        const std::array<std::uint8_t, RedistributionArchiveRecordBytes> bytes =
            encodeRedistributionRecord(row);
        records.append(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()));
        if (records.size() >= 65536)
        {
            if (file.write(records) != records.size())
                throw std::runtime_error("Cannot write redistribution cache records");
            records.clear();
        }
    }
    if (!records.isEmpty() && file.write(records) != records.size())
        throw std::runtime_error("Cannot write redistribution cache records");
    if (cancel_requested != nullptr && cancel_requested->load())
        throw std::runtime_error("Runoff redistribution cancelled");
    if (!file.commit())
        throw std::runtime_error("Cannot commit redistribution cache");
}

// Slow path only: once for each new or invalidated scenario cache.
void calculateAndCacheRedistribution(const QString &path,
                                     const RedistributionArchiveIdentity &identity,
                                     const QString &job_folder,
                                     const QString &scenario_folder,
                                     const QJsonObject &manifest,
                                     const ElevationGrid &geometry,
                                     const std::atomic<bool> *cancel_requested)
{
    const std::uint64_t cells = std::uint64_t(geometry.columns) * geometry.rows;
    QFile raster(QDir(scenario_folder).filePath(QStringLiteral("runoff.f64")));
    if (!raster.open(QIODevice::ReadOnly) || raster.size() != qint64(cells * 8))
        throw std::runtime_error("Missing or truncated rainfall scenario runoff raster");
    std::vector<double> volume(std::size_t(cells), 0.0);
    QByteArray buffer(32768, '\0');
    std::uint64_t cell = 0;
    while (cell < cells)
    {
        if (cancel_requested != nullptr && cancel_requested->load())
            throw std::runtime_error("Runoff redistribution cancelled");
        const qsizetype take = qsizetype(std::min<std::uint64_t>(4096, cells - cell) * 8);
        if (raster.read(buffer.data(), take) != take)
            throw std::runtime_error("Incomplete rainfall scenario runoff raster");
        for (qsizetype i = 0; i < take; i += 8)
        {
            const quint64 bits = qFromLittleEndian<quint64>(buffer.constData() + i);
            double value = 0.0;
            std::memcpy(&value, &bits, sizeof(value));
            volume[std::size_t(cell++)] = value;
        }
    }
    if (cancel_requested != nullptr && cancel_requested->load())
        throw std::runtime_error("Runoff redistribution cancelled");
    const ElevationGrid source = loadOriginalElevation(job_folder, geometry, cancel_requested);
    if (cancel_requested != nullptr && cancel_requested->load())
        throw std::runtime_error("Runoff redistribution cancelled");
    const DepressionHierarchy hierarchy = loadHierarchy(job_folder,
        geometry.columns, geometry.rows);
    const RunoffRedistribution result = redistributeRunoff(source, hierarchy, volume,
                                                            cancel_requested);
    const double expected = manifest.value(QStringLiteral("effective_runoff_volume_m3")).toDouble();
    if (std::abs(expected - result.generated_runoff_m3) >
        1e-7 * std::max(1.0, std::abs(expected)))
        throw std::runtime_error("Rainfall runoff raster and scenario summary disagree");
    writeRedistributionCache(path, identity, result, hierarchy, cancel_requested);
}
}

QJsonObject AnalysisService::rainfallRedistribution(
    const QString &id, const QString &scenario_uuid,
    int offset, int limit, QString *error) const
{
    return rainfallRedistributionImpl(id, scenario_uuid, offset, limit, true, nullptr, error);
}

QJsonObject AnalysisService::rainfallRedistributionReady(
    const QString &id, const QString &scenario_uuid,
    int offset, int limit, QString *error) const
{
    return rainfallRedistributionImpl(id, scenario_uuid, offset, limit, false, nullptr, error);
}

QJsonObject AnalysisService::rainfallRedistributionImpl(
    const QString &id, const QString &scenario_uuid, int offset, int limit,
    bool allow_calculation, const std::atomic<bool> *cancel_requested,
    QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    if (offset < 0 || limit < 1 || limit > 100)
    {
        if (error != nullptr) *error = QStringLiteral("Invalid redistribution pagination (limit 1..100)");
        return {};
    }
    try
    {
        const QString folder = jobDirectory(id);
        const QString scenario_root = QDir(folder).filePath(QStringLiteral("rainfall-scenarios"));
        const QJsonObject manifest = loadRainfallManifest(scenario_root, scenario_uuid);
        const QString scenario_folder = QDir(scenario_root).filePath(scenario_uuid);
        const QString path = QDir(scenario_folder).filePath(QStringLiteral("redistribution-v1.aowbin"));
        const RedistributionArchiveIdentity identity = redistributionIdentity(
            id, scenario_uuid, folder, scenario_folder, item->grid.columns, item->grid.rows);

        // Computations serialize through one process-local mutex, but the
        // cache-only reader never waits for an active calculation. QSaveFile
        // publishes atomically, so concurrent readers see a complete file.
        std::unique_lock<QMutex> locker(this->redistribution_cache_mutex, std::defer_lock);
        if (allow_calculation) locker.lock();
        if (cancel_requested != nullptr && cancel_requested->load())
            throw std::runtime_error("Runoff redistribution cancelled");
        CachedRedistributionPage page;
        bool cache_hit = false;
        const double expected_runoff =
            manifest.value(QStringLiteral("effective_runoff_volume_m3")).toDouble();
        if (QFileInfo::exists(path))
        {
            try
            {
                page = loadRedistributionPage(path, identity, offset, limit);
                if (std::abs(page.header.summary.generated_runoff_m3 - expected_runoff) >
                    1e-7 * std::max(1.0, std::abs(expected_runoff)))
                    throw std::runtime_error("Redistribution cache scenario totals disagree");
                cache_hit = true;
            }
            catch (const std::exception &)
            {
                // A missing/stale/corrupt derived file is disposable. The
                // immutable scenario and DEM remain the authoritative inputs.
            }
        }
        if (!cache_hit)
        {
            if (!allow_calculation)
                throw std::runtime_error("Redistribution result not ready; submit the calculation first");
            calculateAndCacheRedistribution(path, identity, folder, scenario_folder,
                                             manifest, item->grid, cancel_requested);
            page = loadRedistributionPage(path, identity, offset, limit);
        }
        if (std::abs(page.header.summary.generated_runoff_m3 - expected_runoff) >
            1e-7 * std::max(1.0, std::abs(expected_runoff)))
            throw std::runtime_error("Cached runoff volume disagrees with scenario summary");
        QJsonArray paginated;
        for (const RedistributionArchiveRecord &row : page.records)
        {
            const BasinRedistributionState &state = row.state;
            paginated.append(QJsonObject{
                {QStringLiteral("basin_id"), basinUuid(id, state.basin_index)},
                {QStringLiteral("parent_id"), row.parent_index >= 0
                    ? QJsonValue(basinUuid(id, std::uint64_t(row.parent_index)))
                    : QJsonValue(QJsonValue::Null)},
                {QStringLiteral("direct_inflow_m3"), state.direct_inflow_m3},
                {QStringLiteral("stored_in_subtree_m3"), state.stored_in_subtree_m3},
                {QStringLiteral("overflow_m3"), state.overflow_m3},
                {QStringLiteral("at_spill"), state.at_spill}
            });
        }
        const RunoffRedistribution &balance = page.header.summary;
        const std::uint64_t count = page.header.basin_count;
        return QJsonObject{
            {QStringLiteral("model"), QStringLiteral("static_fill_spill_merge_volume_v1")},
            {QStringLiteral("job_id"), id},
            {QStringLiteral("scenario_id"), scenario_uuid},
            {QStringLiteral("generated_runoff_m3"), balance.generated_runoff_m3},
            {QStringLiteral("basin_storage_m3"), balance.basin_storage_m3},
            {QStringLiteral("direct_boundary_outflow_m3"), balance.direct_boundary_outflow_m3},
            {QStringLiteral("basin_boundary_outflow_m3"), balance.basin_boundary_outflow_m3},
            {QStringLiteral("total_boundary_outflow_m3"), balance.total_boundary_outflow_m3},
            {QStringLiteral("mass_balance_error_m3"), balance.mass_balance_error_m3},
            {QStringLiteral("domain_boundary_is_artificial"), balance.domain_boundary_is_artificial},
            {QStringLiteral("cache_hit"), cache_hit},
            {QStringLiteral("total_basins"), double(count)},
            {QStringLiteral("offset"), offset},
            {QStringLiteral("limit"), limit},
            {QStringLiteral("has_more"), std::uint64_t(offset) +
                std::uint64_t(paginated.size()) < count},
            {QStringLiteral("basins"), paginated}
        };
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

namespace
{
QString redistributionTaskKey(const QString &analysis_id, const QString &scenario_id)
{
    return analysis_id + QLatin1Char('/') + scenario_id;
}

QJsonObject redistributionTaskJson(const QString &analysis_id, const QString &scenario_id,
                                   const QString &task_id, const QString &state,
                                   const QString &message, bool cached)
{
    return QJsonObject{
        {QStringLiteral("id"), task_id.isEmpty() ? QJsonValue(QJsonValue::Null)
                                               : QJsonValue(task_id)},
        {QStringLiteral("job_id"), analysis_id},
        {QStringLiteral("scenario_id"), scenario_id},
        {QStringLiteral("state"), state},
        {QStringLiteral("error"), message},
        {QStringLiteral("cache_ready"), cached}
    };
}
} // namespace

QJsonObject AnalysisService::rainfallRedistributionStatus(
    const QString &id, const QString &scenario_uuid, QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    try
    {
        const QString folder = jobDirectory(id);
        const QString scenario_root = QDir(folder).filePath(QStringLiteral("rainfall-scenarios"));
        const QJsonObject manifest = loadRainfallManifest(scenario_root, scenario_uuid);
        const QString scenario_folder = QDir(scenario_root).filePath(scenario_uuid);
        const RedistributionArchiveIdentity identity = redistributionIdentity(
            id, scenario_uuid, folder, scenario_folder, item->grid.columns, item->grid.rows);
        bool cache_ready = false;
        try
        {
            const CachedRedistributionPage cached = loadRedistributionPage(
                QDir(scenario_folder).filePath(QStringLiteral("redistribution-v1.aowbin")),
                identity, 0, 1);
            const double expected = manifest.value(QStringLiteral("effective_runoff_volume_m3")).toDouble();
            cache_ready = std::abs(cached.header.summary.generated_runoff_m3 - expected) <=
                1e-7 * std::max(1.0, std::abs(expected));
        }
        catch (const std::exception &)
        {
            // Missing/invalid derived data is reported as not ready.
        }
        QString task_id;
        QString state = QStringLiteral("not_started");
        QString message;
        {
            QMutexLocker locker(&this->redistribution_tasks_mutex);
            const QSharedPointer<RedistributionTask> task = this->redistribution_tasks.value(
                redistributionTaskKey(id, scenario_uuid));
            if (task)
            {
                task_id = task->id;
                state = task->state;
                message = task->error;
            }
        }
        if (cache_ready)
        {
            state = QStringLiteral("completed");
            message.clear();
        }
        else if (state == QStringLiteral("completed"))
            state = QStringLiteral("not_started"); // cache was invalidated after completion
        return redistributionTaskJson(id, scenario_uuid, task_id, state, message, cache_ready);
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

QJsonObject AnalysisService::startRainfallRedistribution(
    const QString &id, const QString &scenario_uuid, QString *error)
{
    const QJsonObject current = rainfallRedistributionStatus(id, scenario_uuid, error);
    if (current.isEmpty()) return {};
    if (current.value(QStringLiteral("cache_ready")).toBool()) return current;
    const QString key = redistributionTaskKey(id, scenario_uuid);
    QSharedPointer<RedistributionTask> task;
    {
        QMutexLocker locker(&this->redistribution_tasks_mutex);
        const QSharedPointer<RedistributionTask> existing = this->redistribution_tasks.value(key);
        if (existing && (existing->state == QStringLiteral("queued") ||
                         existing->state == QStringLiteral("running") ||
                         existing->state == QStringLiteral("cancel_requested")))
            return redistributionTaskJson(id, scenario_uuid, existing->id,
                                          existing->state, existing->error, false);
        if (existing && existing->state == QStringLiteral("completed"))
        {
            // The cache might have been published after the initial status
            // check, but before this lock was acquired. A completed worker
            // has already committed its result. Revalidate that result before
            // replacing its UUID; invalidated caches must still regenerate.
            try
            {
                const QSharedPointer<Job> job_item = findJob(id);
                const QString folder = jobDirectory(id);
                const QString scenario_root = QDir(folder).filePath(
                    QStringLiteral("rainfall-scenarios"));
                const QJsonObject rainfall = loadRainfallManifest(
                    scenario_root, scenario_uuid);
                const QString scenario_folder = QDir(scenario_root).filePath(scenario_uuid);
                const RedistributionArchiveIdentity identity = redistributionIdentity(
                    id, scenario_uuid, folder, scenario_folder,
                    job_item->grid.columns, job_item->grid.rows);
                const CachedRedistributionPage cached = loadRedistributionPage(
                    QDir(scenario_folder).filePath(QStringLiteral("redistribution-v1.aowbin")),
                    identity, 0, 1);
                const double expected = rainfall.value(
                    QStringLiteral("effective_runoff_volume_m3")).toDouble();
                if (std::abs(cached.header.summary.generated_runoff_m3 - expected) <=
                    1e-7 * std::max(1.0, std::abs(expected)))
                    return redistributionTaskJson(id, scenario_uuid, existing->id,
                        QStringLiteral("completed"), QString(), true);
            }
            catch (const std::exception &)
            {
                // A stale or damaged result is not reusable.
            }
        }
        std::size_t pending = 0;
        for (const QSharedPointer<RedistributionTask> &entry :
             std::as_const(this->redistribution_tasks))
            if (entry->state == QStringLiteral("queued") ||
                entry->state == QStringLiteral("running") ||
                entry->state == QStringLiteral("cancel_requested"))
                ++pending;
        if (pending >= 8)
        {
            if (error != nullptr) *error = QStringLiteral("Redistribution queue full (8 active/queued jobs)");
            return {};
        }
        task = QSharedPointer<RedistributionTask>::create();
        task->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        task->analysis_id = id;
        task->scenario_id = scenario_uuid;
        this->redistribution_tasks.insert(key, task);
    }
    this->redistribution_workers.start([this, task]() { runRedistributionTask(task); });
    return redistributionTaskJson(id, scenario_uuid, task->id, QStringLiteral("queued"),
                                  QString(), false);
}

void AnalysisService::runRedistributionTask(const QSharedPointer<RedistributionTask> &task)
{
    {
        QMutexLocker locker(&this->redistribution_tasks_mutex);
        if (!task->cancelled.load()) task->state = QStringLiteral("running");
    }
    QString error;
    if (!task->cancelled.load())
        rainfallRedistributionImpl(task->analysis_id, task->scenario_id, 0, 1,
                                   true, &task->cancelled, &error);
    {
        QMutexLocker locker(&this->redistribution_tasks_mutex);
        if (task->cancelled.load())
        {
            task->state = QStringLiteral("canceled");
            task->error.clear();
        }
        else if (!error.isEmpty())
        {
            task->state = QStringLiteral("failed");
            task->error = error;
        }
        else
        {
            task->state = QStringLiteral("completed");
            task->error.clear();
        }
    }
}

bool AnalysisService::cancelRainfallRedistribution(
    const QString &id, const QString &scenario_uuid, QString *error)
{
    if (error != nullptr) error->clear();
    const QString key = redistributionTaskKey(id, scenario_uuid);
    QMutexLocker locker(&this->redistribution_tasks_mutex);
    const QSharedPointer<RedistributionTask> task = this->redistribution_tasks.value(key);
    if (!task || (task->state != QStringLiteral("running") &&
                  task->state != QStringLiteral("queued") &&
                  task->state != QStringLiteral("cancel_requested")))
    {
        if (error != nullptr) *error = QStringLiteral("No active redistribution task");
        return false;
    }
    task->cancelled.store(true);
    task->state = QStringLiteral("cancel_requested");
    return true;
}
} // namespace Aowis::Hydrology

namespace Aowis::Hydrology
{
namespace
{
// The derived depth file is published first and its manifest last. Both use
// QSaveFile; the manifest is the only commit marker. A content-derived
// fingerprint follows DEM, rainfall, and redistribution cache bytes.
QString inundationFile(const QString &scenario_folder, const QString &name)
{
    return QDir(scenario_folder).filePath(name);
}

QString inundationFingerprint(const QString &job_id, const QString &scenario_id,
                              const QString &job_folder, const QString &scenario_folder,
                              std::uint32_t columns, std::uint32_t rows)
{
    const RedistributionArchiveIdentity identity = redistributionIdentity(job_id,
        scenario_id, job_folder, scenario_folder, columns, rows);
    const QString redistribution_path = inundationFile(scenario_folder,
        QStringLiteral("redistribution-v1.aowbin"));
    const QFileInfo info(redistribution_path);
    if (!info.isFile() || info.size() < qint64(RedistributionArchiveHeaderBytes))
        throw std::runtime_error("Redistribution result not ready; submit redistribution first");
    QByteArray contents(reinterpret_cast<const char *>(identity.source_fingerprint.data()),
                        qsizetype(identity.source_fingerprint.size()));
    contents += QByteArrayLiteral(":redistribution-sha256:") +
        sourceDigest(redistribution_path).toLatin1();
    contents += QByteArrayLiteral(":static-inundation-v2:content-fingerprint-v2");
    return QString::fromLatin1(QCryptographicHash::hash(contents,
        QCryptographicHash::Sha256).toHex());
}

QJsonObject cachedInundationManifest(const QString &scenario_folder,
                                    const QString &fingerprint,
                                    std::uint32_t columns, std::uint32_t rows)
{
    QFile file(inundationFile(scenario_folder, QStringLiteral("inundation-v2.json")));
    if (!file.open(QIODevice::ReadOnly) || file.size() < 10 || file.size() > 8000000)
        throw std::runtime_error("Inundation result not ready; submit reconstruction first");
    const QByteArray bytes = file.readAll();
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error("Invalid inundation cache manifest");
    const QJsonObject manifest = document.object();
    const QString status = manifest.value(QStringLiteral("inundation_status")).toString();
    if (manifest.value(QStringLiteral("schema")).toString() !=
            QStringLiteral("aowis-static-inundation-v2") ||
        manifest.value(QStringLiteral("source_fingerprint")).toString() != fingerprint ||
        manifest.value(QStringLiteral("columns")).toInt() != int(columns) ||
        manifest.value(QStringLiteral("rows")).toInt() != int(rows) ||
        (status != QStringLiteral("available") &&
         status != QStringLiteral("unresolved_partial_merge")) ||
        !manifest.value(QStringLiteral("allocation_assumed")).isBool())
        throw std::runtime_error("Stale or invalid inundation cache manifest");
    if (status == QStringLiteral("available"))
    {
        const QJsonArray row_hashes = manifest.value(QStringLiteral("row_sha256")).toArray();
        const QFileInfo depth(inundationFile(scenario_folder, QStringLiteral("inundation-v2.f32")));
        if (row_hashes.size() != int(rows) || !depth.isFile() ||
            depth.size() != qint64(std::uint64_t(columns) * rows * 4))
            throw std::runtime_error("Incomplete inundation depth cache");
    }
    return manifest;
}

QJsonObject inundationTaskJson(const QString &job_id, const QString &scenario_id,
                               const QString &task_id, const QString &state,
                               const QString &error, const QJsonObject &manifest)
{
    QJsonObject output{
        {QStringLiteral("job_id"), job_id},
        {QStringLiteral("scenario_id"), scenario_id},
        {QStringLiteral("id"), task_id.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                  : QJsonValue(task_id)},
        {QStringLiteral("state"), state},
        {QStringLiteral("error"), error},
        {QStringLiteral("cache_ready"), !manifest.isEmpty()},
        {QStringLiteral("inundation_status"), manifest.isEmpty()
            ? QJsonValue(QJsonValue::Null)
            : manifest.value(QStringLiteral("inundation_status"))},
        {QStringLiteral("depth_available"), manifest.value(
            QStringLiteral("inundation_status")) == QStringLiteral("available")},
        {QStringLiteral("allocation_assumed"), manifest.value(
            QStringLiteral("allocation_assumed")).toBool()},
        {QStringLiteral("model"), QStringLiteral("static_standing_water_v2")}
    };
    if (!manifest.isEmpty())
    {
        output.insert(QStringLiteral("reconstructed_volume_m3"),
            manifest.value(QStringLiteral("reconstructed_volume_m3")));
        output.insert(QStringLiteral("inundated_area_m2"),
            manifest.value(QStringLiteral("inundated_area_m2")));
        output.insert(QStringLiteral("inundated_cells"),
            manifest.value(QStringLiteral("inundated_cells")));
        output.insert(QStringLiteral("surfaces"), manifest.value(QStringLiteral("surfaces")));
        output.insert(QStringLiteral("columns"), manifest.value(QStringLiteral("columns")));
        output.insert(QStringLiteral("rows"), manifest.value(QStringLiteral("rows")));
    }
    return output;
}

void calculateInundationCache(const QString &job_id, const QString &scenario_id,
                              const QString &job_folder, const QString &scenario_folder,
                              const ElevationGrid &geometry, std::uint64_t maximum_cells,
                              const std::atomic<bool> *cancel_requested)
{
    const QString fingerprint = inundationFingerprint(job_id, scenario_id, job_folder,
        scenario_folder, geometry.columns, geometry.rows);
    const RedistributionArchiveIdentity identity = redistributionIdentity(job_id,
        scenario_id, job_folder, scenario_folder, geometry.columns, geometry.rows);
    const DepressionHierarchy hierarchy = loadHierarchy(job_folder,
        geometry.columns, geometry.rows);
    if (hierarchy.basins.size() > std::size_t(std::numeric_limits<int>::max()))
        throw std::runtime_error("Too many basins for inundation reconstruction");
    const CachedRedistributionPage cached = loadRedistributionPage(
        inundationFile(scenario_folder, QStringLiteral("redistribution-v1.aowbin")),
        identity, 0, int(hierarchy.basins.size()));
    if (cached.header.basin_count != hierarchy.basins.size() ||
        cached.records.size() != hierarchy.basins.size())
        throw std::runtime_error("Inundation and redistribution basin counts disagree");
    const QJsonObject scenario = loadRainfallManifest(
        QDir(job_folder).filePath(QStringLiteral("rainfall-scenarios")), scenario_id);
    const double expected = scenario.value(QStringLiteral("effective_runoff_volume_m3")).toDouble();
    if (std::abs(expected - cached.header.summary.generated_runoff_m3) >
        1e-7 * std::max(1.0, std::abs(expected)))
        throw std::runtime_error("Inundation source runoff total disagrees with scenario");
    RunoffRedistribution redistribution = cached.header.summary;
    redistribution.basins.reserve(cached.records.size());
    for (std::size_t index = 0; index < cached.records.size(); ++index)
    {
        if ((index & 4095U) == 0 && cancel_requested != nullptr && cancel_requested->load())
            throw std::runtime_error("Static inundation reconstruction canceled");
        redistribution.basins.push_back(cached.records[index].state);
    }
    const ElevationGrid elevation = loadOriginalElevation(job_folder, geometry, cancel_requested);
    const StaticInundation result = reconstructStaticInundation(elevation, hierarchy,
        redistribution, maximum_cells, cancel_requested);
    if (cancel_requested != nullptr && cancel_requested->load())
        throw std::runtime_error("Static inundation reconstruction canceled");

    const QString status = result.status == InundationStatus::Available
        ? QStringLiteral("available") : QStringLiteral("unresolved_partial_merge");
    QJsonArray surfaces;
    for (const BasinWaterSurface &surface : result.surfaces)
        surfaces.append(QJsonObject{
            {QStringLiteral("basin_id"), basinUuid(job_id, surface.basin_index)},
            {QStringLiteral("water_level_m"), surface.water_level_m},
            {QStringLiteral("stored_volume_m3"), surface.stored_volume_m3},
            {QStringLiteral("inundated_area_m2"), surface.inundated_area_m2},
            {QStringLiteral("inundated_cells"), double(surface.inundated_cells)}
        });
    QJsonArray hashes;
    if (result.status == InundationStatus::Available)
    {
        if (geometry.rows > 100000 || result.depth_m.size() !=
            std::size_t(std::uint64_t(geometry.columns) * geometry.rows))
            throw std::runtime_error("Inundation cache row budget exceeded");
        QSaveFile depth(inundationFile(scenario_folder, QStringLiteral("inundation-v2.f32")));
        if (!depth.open(QIODevice::WriteOnly))
            throw std::runtime_error("Cannot create inundation depth raster");
        QByteArray row(qsizetype(geometry.columns) * 4, '\0');
        for (std::uint32_t y = 0; y < geometry.rows; ++y)
        {
            if (cancel_requested != nullptr && cancel_requested->load())
                throw std::runtime_error("Static inundation reconstruction canceled");
            for (std::uint32_t x = 0; x < geometry.columns; ++x)
            {
                const float value = result.depth_m[std::size_t(y) * geometry.columns + x];
                quint32 bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                qToLittleEndian(bits, row.data() + qsizetype(x) * 4);
            }
            hashes.append(QString::fromLatin1(QCryptographicHash::hash(row,
                QCryptographicHash::Sha256).toHex()));
            if (depth.write(row) != row.size())
                throw std::runtime_error("Cannot write inundation depth raster");
        }
        if (!depth.commit())
            throw std::runtime_error("Cannot publish inundation depth raster");
    }
    if (cancel_requested != nullptr && cancel_requested->load())
        throw std::runtime_error("Static inundation reconstruction canceled");
    const QJsonObject manifest{
        {QStringLiteral("schema"), QStringLiteral("aowis-static-inundation-v2")},
        {QStringLiteral("source_fingerprint"), fingerprint},
        {QStringLiteral("columns"), int(geometry.columns)},
        {QStringLiteral("rows"), int(geometry.rows)},
        {QStringLiteral("inundation_status"), status},
        {QStringLiteral("allocation_assumed"), result.allocation_assumed},
        {QStringLiteral("reconstructed_volume_m3"), result.reconstructed_volume_m3},
        {QStringLiteral("inundated_area_m2"), result.inundated_area_m2},
        {QStringLiteral("inundated_cells"), double(result.inundated_cells)},
        {QStringLiteral("surfaces"), surfaces},
        {QStringLiteral("row_sha256"), hashes}
    };
    const QByteArray encoded = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    if (encoded.size() > 8000000)
        throw std::runtime_error("Inundation metadata exceeds 8 MiB limit");
    QSaveFile output(inundationFile(scenario_folder, QStringLiteral("inundation-v2.json")));
    if (!output.open(QIODevice::WriteOnly) || output.write(encoded) != encoded.size() ||
        !output.commit())
        throw std::runtime_error("Cannot publish inundation cache manifest");
}

} // namespace

QJsonObject AnalysisService::staticInundationStatus(
    const QString &id, const QString &scenario_uuid, QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    try
    {
        const QString job_folder = jobDirectory(id);
        const QString scenario_root = QDir(job_folder).filePath(QStringLiteral("rainfall-scenarios"));
        (void)loadRainfallManifest(scenario_root, scenario_uuid);
        const QString scenario_folder = QDir(scenario_root).filePath(scenario_uuid);
        QJsonObject manifest;
        try
        {
            const QString fingerprint = inundationFingerprint(id, scenario_uuid,
                job_folder, scenario_folder, item->grid.columns, item->grid.rows);
            const RedistributionArchiveIdentity identity = redistributionIdentity(
                id, scenario_uuid, job_folder, scenario_folder, item->grid.columns, item->grid.rows);
            (void)loadRedistributionPage(inundationFile(scenario_folder,
                QStringLiteral("redistribution-v1.aowbin")), identity, 0, 0);
            manifest = cachedInundationManifest(scenario_folder, fingerprint,
                item->grid.columns, item->grid.rows);
        }
        catch (const std::exception &)
        {
            // A missing, stale or malformed cache is a cache miss. POST can
            // regenerate it once the redistribution source is ready.
        }
        QString task_id;
        QString state = QStringLiteral("not_started");
        QString message;
        {
            QMutexLocker locker(&this->redistribution_tasks_mutex);
            const QSharedPointer<InundationTask> task = this->inundation_tasks.value(
                redistributionTaskKey(id, scenario_uuid));
            if (task)
            {
                task_id = task->id;
                state = task->state;
                message = task->error;
            }
        }
        if (!manifest.isEmpty())
        {
            state = QStringLiteral("completed");
            message.clear();
        }
        else if (state == QStringLiteral("completed")) state = QStringLiteral("not_started");
        return inundationTaskJson(id, scenario_uuid, task_id, state, message, manifest);
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

QJsonObject AnalysisService::startStaticInundation(
    const QString &id, const QString &scenario_uuid, QString *error)
{
    const QJsonObject current = staticInundationStatus(id, scenario_uuid, error);
    if (current.isEmpty()) return {};
    if (current.value(QStringLiteral("cache_ready")).toBool()) return current;
    try
    {
        const QSharedPointer<Job> job_item = findJob(id);
        const QString folder = jobDirectory(id);
        const QString scenario_folder = QDir(folder).filePath(
            QStringLiteral("rainfall-scenarios/") + scenario_uuid);
        const RedistributionArchiveIdentity identity = redistributionIdentity(id, scenario_uuid,
            folder, scenario_folder, job_item->grid.columns, job_item->grid.rows);
        const CachedRedistributionPage page = loadRedistributionPage(
            inundationFile(scenario_folder, QStringLiteral("redistribution-v1.aowbin")),
            identity, 0, 0);
        const QJsonObject rainfall = loadRainfallManifest(
            QDir(folder).filePath(QStringLiteral("rainfall-scenarios")), scenario_uuid);
        const double expected = rainfall.value(QStringLiteral("effective_runoff_volume_m3")).toDouble();
        if (std::abs(expected - page.header.summary.generated_runoff_m3) >
            1e-7 * std::max(1.0, std::abs(expected)))
            throw std::runtime_error("Redistribution result not ready; source totals disagree");
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
    const QString key = redistributionTaskKey(id, scenario_uuid);
    QSharedPointer<InundationTask> task;
    {
        QMutexLocker locker(&this->redistribution_tasks_mutex);
        const QSharedPointer<InundationTask> existing = this->inundation_tasks.value(key);
        if (existing && (existing->state == QStringLiteral("queued") ||
                         existing->state == QStringLiteral("running") ||
                         existing->state == QStringLiteral("cancel_requested")))
            return inundationTaskJson(id, scenario_uuid, existing->id, existing->state,
                                      existing->error, {});
        if (existing && existing->state == QStringLiteral("completed"))
        {
            // The cache can be committed between the initial status lookup
            // and the task-table lookup. Keep the successful worker's UUID if
            // its result is still valid, even for a very fast reconstruction.
            try
            {
                const QSharedPointer<Job> job_item = findJob(id);
                const QString folder = jobDirectory(id);
                const QString scenario_folder = QDir(folder).filePath(
                    QStringLiteral("rainfall-scenarios/") + scenario_uuid);
                const QString fingerprint = inundationFingerprint(id, scenario_uuid,
                    folder, scenario_folder, job_item->grid.columns, job_item->grid.rows);
                const QJsonObject manifest = cachedInundationManifest(scenario_folder,
                    fingerprint, job_item->grid.columns, job_item->grid.rows);
                return inundationTaskJson(id, scenario_uuid, existing->id,
                    QStringLiteral("completed"), QString(), manifest);
            }
            catch (const std::exception &)
            {
                // A missing, stale or damaged result must be regenerated.
            }
        }
        std::size_t pending = 0;
        for (const QSharedPointer<InundationTask> &entry : std::as_const(this->inundation_tasks))
            if (entry->state == QStringLiteral("queued") ||
                entry->state == QStringLiteral("running") ||
                entry->state == QStringLiteral("cancel_requested")) ++pending;
        if (pending >= 8)
        {
            if (error != nullptr) *error = QStringLiteral("Inundation queue full (8 active/queued jobs)");
            return {};
        }
        task = QSharedPointer<InundationTask>::create();
        task->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        task->analysis_id = id;
        task->scenario_id = scenario_uuid;
        this->inundation_tasks.insert(key, task);
    }
    this->redistribution_workers.start([this, task]() { runInundationTask(task); });
    return inundationTaskJson(id, scenario_uuid, task->id, QStringLiteral("queued"),
                               QString(), {});
}

void AnalysisService::runInundationTask(const QSharedPointer<InundationTask> &task)
{
    {
        QMutexLocker locker(&this->redistribution_tasks_mutex);
        if (!task->cancelled.load()) task->state = QStringLiteral("running");
    }
    QString error;
    try
    {
        if (!task->cancelled.load())
        {
            const QSharedPointer<Job> job_item = findJob(task->analysis_id);
            if (!job_item) throw std::runtime_error("Unknown terrain job");
            const QString folder = jobDirectory(task->analysis_id);
            const QString scenario_folder = QDir(folder).filePath(
                QStringLiteral("rainfall-scenarios/") + task->scenario_id);
            calculateInundationCache(task->analysis_id, task->scenario_id, folder,
                scenario_folder, job_item->grid, this->config.maximum_cells, &task->cancelled);
        }
    }
    catch (const std::exception &ex) { error = QString::fromUtf8(ex.what()); }
    {
        QMutexLocker locker(&this->redistribution_tasks_mutex);
        if (task->cancelled.load())
        {
            task->state = QStringLiteral("canceled");
            task->error.clear();
        }
        else if (!error.isEmpty())
        {
            task->state = QStringLiteral("failed");
            task->error = error;
        }
        else
        {
            task->state = QStringLiteral("completed");
            task->error.clear();
        }
    }
}

bool AnalysisService::cancelStaticInundation(
    const QString &id, const QString &scenario_uuid, QString *error)
{
    if (error != nullptr) error->clear();
    const QString key = redistributionTaskKey(id, scenario_uuid);
    QMutexLocker locker(&this->redistribution_tasks_mutex);
    const QSharedPointer<InundationTask> task = this->inundation_tasks.value(key);
    if (!task || (task->state != QStringLiteral("queued") &&
                  task->state != QStringLiteral("running") &&
                  task->state != QStringLiteral("cancel_requested")))
    {
        if (error != nullptr) *error = QStringLiteral("No active inundation task");
        return false;
    }
    task->cancelled.store(true);
    task->state = QStringLiteral("cancel_requested");
    return true;
}

QByteArray AnalysisService::staticInundationDepthWindow(
    const QString &id, const QString &scenario_uuid,
    int x, int y, int width, int height, QString *error) const
{
    if (error != nullptr) error->clear();
    const QSharedPointer<Job> item = completedJob(id, error);
    if (!item) return {};
    if (x < 0 || y < 0 || width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        std::uint64_t(width) * height > 1000000 ||
        std::uint64_t(x) + width > item->grid.columns ||
        std::uint64_t(y) + height > item->grid.rows)
    {
        if (error != nullptr) *error = QStringLiteral("Invalid inundation raster window");
        return {};
    }
    try
    {
        const QString job_folder = jobDirectory(id);
        const QString scenario_root = QDir(job_folder).filePath(QStringLiteral("rainfall-scenarios"));
        (void)loadRainfallManifest(scenario_root, scenario_uuid);
        const QString scenario_folder = QDir(scenario_root).filePath(scenario_uuid);
        const QString fingerprint = inundationFingerprint(id, scenario_uuid,
            job_folder, scenario_folder, item->grid.columns, item->grid.rows);
        const QJsonObject manifest = cachedInundationManifest(scenario_folder, fingerprint,
            item->grid.columns, item->grid.rows);
        if (manifest.value(QStringLiteral("inundation_status")) != QStringLiteral("available"))
            throw std::runtime_error("Inundation unavailable: unresolved partial basin merge");
        QFile file(inundationFile(scenario_folder, QStringLiteral("inundation-v2.f32")));
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error("Inundation depth cache not ready");
        QByteArray output(qsizetype(width) * height * 4, '\0');
        const QJsonArray hashes = manifest.value(QStringLiteral("row_sha256")).toArray();
        for (int row_index = 0; row_index < height; ++row_index)
        {
            const int source_row = y + row_index;
            if (!file.seek(qint64(std::uint64_t(source_row) * item->grid.columns * 4)))
                throw std::runtime_error("Invalid inundation cache row offset");
            const QByteArray row = file.read(qsizetype(item->grid.columns) * 4);
            if (row.size() != qsizetype(item->grid.columns) * 4 ||
                QString::fromLatin1(QCryptographicHash::hash(row,
                    QCryptographicHash::Sha256).toHex()) != hashes.at(source_row).toString())
            {
                // On-demand row checks prevent undetected damage in returned
                // windows. A bad cache is disposable; POST regenerates it.
                file.close();
                QFile::remove(inundationFile(scenario_folder, QStringLiteral("inundation-v2.json")));
                throw std::runtime_error("Invalid inundation cache row checksum; resubmit reconstruction");
            }
            const qsizetype take = qsizetype(width) * 4;
            std::memcpy(output.data() + qsizetype(row_index) * take,
                row.constData() + qsizetype(x) * 4, std::size_t(take));
        }
        return output;
    }
    catch (const std::exception &ex)
    {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
        return {};
    }
}

} // namespace Aowis::Hydrology
