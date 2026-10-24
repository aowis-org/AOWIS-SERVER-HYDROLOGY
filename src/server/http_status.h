#ifndef AOWIS_HYDROLOGY_HTTP_STATUS_H
#define AOWIS_HYDROLOGY_HTTP_STATUS_H

// Transport-only mapping of AnalysisService error messages to HTTP status
// codes. Header-only and Qt Core only, so it is unit-tested directly.
#include <QLatin1String>
#include <QString>

namespace Aowis::Hydrology
{
enum class HttpStatus : int
{
    Ok = 200, Created = 201, Accepted = 202, BadRequest = 400,
    Unauthorized = 401, NotFound = 404, Conflict = 409, PayloadTooLarge = 413,
    TooManyRequests = 429, InternalServerError = 500, ServiceUnavailable = 503
};

// Single place that maps service error messages to HTTP status codes. The
// service API reports errors as text; every route uses this table with its
// own fallback, so a message category means the same status everywhere:
//   429 capacity/busy/queue full      503 restored artifacts being verified
//   409 not completed / not ready / no active task
//   404 unknown entities              400 rejected input
//   500 storage corruption and "Cannot ..." I/O failures
// Messages matching no rule keep the route's fallback status.
inline HttpStatus statusForError(const QString &error, HttpStatus fallback)
{
    const auto starts = [&error](const char *prefix)
    {
        return error.startsWith(QLatin1String(prefix));
    };
    if (error.contains(QLatin1String("queue full")) || starts("Heavy query capacity busy") ||
        starts("Catchment calculation busy") || starts("Terrain analysis capacity"))
        return HttpStatus::TooManyRequests;
    if (starts("Analysis integrity verification in progress"))
        return HttpStatus::ServiceUnavailable;
    if (starts("Unknown or incomplete job") || starts("Analysis not completed") ||
        error.contains(QLatin1String("not ready")) || starts("No active") ||
        starts("Inundation unavailable"))
        return HttpStatus::Conflict;
    if (starts("Unknown"))
        return HttpStatus::NotFound;
    // Integrity failures of stored/derived files are server-side faults even
    // though their messages begin with "Invalid".
    for (const char *corruption : {"Invalid completed", "Invalid redistribution cache",
                                   "Invalid inundation cache", "Invalid immutable",
                                   "Invalid cache job", "Invalid coupled simulation identity",
                                   "Invalid basin archive", "Invalid depression hierarchy",
                                   "Corrupt", "Cannot"})
        if (starts(corruption))
            return HttpStatus::InternalServerError;
    for (const char *rejected : {"Invalid", "Too many", "Antimeridian", "Rainfall polygon",
                                 "Water level", "Duplicate", "Coupled DEM cell limit exceeded",
                                 "Basin does not have", "Basin has no known",
                                 "Basin storage query cell limit", "Analysis exceeds",
                                 "Hydraulic control junction"})
        if (starts(rejected))
            return HttpStatus::BadRequest;
    return fallback;
}

} // namespace Aowis::Hydrology
#endif
