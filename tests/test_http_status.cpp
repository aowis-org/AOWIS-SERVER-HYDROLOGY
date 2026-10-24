#ifdef NDEBUG
#undef NDEBUG // Exercise assertions in Release builds, too.
#endif
#include "../src/server/http_status.h"

#include <cassert>
#include <iostream>

using namespace Aowis::Hydrology;

namespace
{
void expect(const char *message, HttpStatus fallback, HttpStatus expected)
{
    const HttpStatus actual = statusForError(QString::fromLatin1(message), fallback);
    if (actual != expected)
    {
        std::cerr << "Unexpected status " << int(actual) << " for: " << message << '\n';
        assert(false);
    }
}
}

int main()
{
    const HttpStatus fallback = HttpStatus::InternalServerError;
    // Capacity is uniformly 429, including the queues formerly answered 409.
    expect("Heavy query capacity busy; retry later", fallback, HttpStatus::TooManyRequests);
    expect("Catchment calculation busy", fallback, HttpStatus::TooManyRequests);
    expect("Terrain analysis capacity reached (8 active/queued jobs)", HttpStatus::BadRequest,
           HttpStatus::TooManyRequests);
    for (const char *queue : {"Redistribution queue full (8 active/queued jobs)",
                              "Inundation queue full (8 active/queued jobs)",
                              "Coupled simulation queue full (8 active/queued)",
                              "Rainfall scenario queue full (8 active/queued tasks)",
                              "Catchment task queue full (4 active/queued tasks)"})
        expect(queue, HttpStatus::Conflict, HttpStatus::TooManyRequests);
    expect("Analysis integrity verification in progress; retry later", fallback,
           HttpStatus::ServiceUnavailable);
    // State conflicts.
    expect("Unknown or incomplete job", fallback, HttpStatus::Conflict);
    expect("Redistribution result not ready; submit the calculation first", fallback,
           HttpStatus::Conflict);
    expect("Catchment result not ready", fallback, HttpStatus::Conflict);
    expect("No active redistribution task", fallback, HttpStatus::Conflict);
    expect("Inundation unavailable: unresolved partial basin merge", fallback,
           HttpStatus::Conflict);
    // Unknown entities.
    expect("Unknown basin UUID", fallback, HttpStatus::NotFound);
    expect("Unknown rainfall scenario task", HttpStatus::Conflict, HttpStatus::NotFound);
    // Rejected input, including messages formerly answered 500.
    expect("Invalid basin water level", fallback, HttpStatus::BadRequest);
    expect("Invalid basin UUID", fallback, HttpStatus::BadRequest);
    expect("Basin storage query cell limit exceeded", fallback, HttpStatus::BadRequest);
    expect("Basin does not have a known outlet within this terrain domain", fallback,
           HttpStatus::BadRequest);
    expect("Water level outside basin minimum/spill interval", fallback, HttpStatus::BadRequest);
    expect("Antimeridian-crossing polygons are not supported", fallback, HttpStatus::BadRequest);
    expect("Coupled DEM cell limit exceeded", fallback, HttpStatus::BadRequest);
    // Server-side corruption and I/O stays 500 despite an "Invalid" prefix.
    expect("Invalid inundation cache row checksum; resubmit reconstruction",
           HttpStatus::BadRequest, HttpStatus::InternalServerError);
    expect("Invalid completed terrain source manifest", HttpStatus::BadRequest,
           HttpStatus::InternalServerError);
    expect("Invalid depression hierarchy basin record", HttpStatus::BadRequest,
           HttpStatus::InternalServerError);
    expect("Corrupt rainfall scenario manifest", HttpStatus::BadRequest,
           HttpStatus::InternalServerError);
    expect("Cannot create job directory", HttpStatus::BadRequest,
           HttpStatus::InternalServerError);
    expect("Rainfall polygon covers no DEM cells", fallback, HttpStatus::BadRequest);
    // Unclassified messages keep the route-specific fallback.
    expect("Something unexpected", HttpStatus::BadRequest, HttpStatus::BadRequest);
    expect("Something unexpected", fallback, fallback);
    std::cout << "HTTP status mapping tests passed\n";
    return 0;
}
