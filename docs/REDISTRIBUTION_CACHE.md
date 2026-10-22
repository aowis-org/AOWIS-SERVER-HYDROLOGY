# Persisted runoff redistribution: format and lifecycle

This document describes the derived-data cache used by
`AnalysisService::rainfallRedistribution()` and the corresponding REST API.
For the hydrological model itself, including its physical limitations and
mass balance, read [RUNOFF_REDISTRIBUTION.md](RUNOFF_REDISTRIBUTION.md).

## Ownership and data flow

**Map** owns terrain acquisition and Copernicus data. Hydrology saves the
original terrain raster with each completed analysis; the terrain source is
not queried again for a rainfall scenario. **Hydrology** owns the original
DEM result, depression hierarchy, rainfall scenario, and redistribution cache.

```text
Map terrain -> completed Hydrology terrain job
                    |-- elevation.f32        (immutable source elevations)
                    |-- basins.aowbin       (immutable depression hierarchy)
                    `-- rainfall-scenarios/<scenario UUID>/
                           |-- scenario.json             (rainfall input and totals)
                           |-- runoff.f64                (m³ per original DEM cell)
                           `-- redistribution-v1.aowbin (derived, disposable cache)
```

The derived file is built on demand at the **first successful redistribution
query** for each rainfall scenario. It is not an independent analysis job or a
new AOWIS database entity. Changing query pagination never alters the stored
physical result. Each scenario has its own cache; it cannot be shared across
jobs or rainfall scenarios. Scenario and job IDs are UUIDs.

## Request lifecycle

1. Verify that the terrain analysis is completed, the requested scenario UUID
   exists, and `offset >= 0`, `1 <= limit <= 100`.
2. Read the scenario manifest and a **cheap metadata fingerprint** for the four
   immutable inputs (original elevation, basin archive, runoff, scenario manifest).
3. If the cache exists and matches the current job/scenario UUIDs, grid
   dimensions, file fingerprint and archive version, validate its header and
   the requested page of records. Return the global totals and only those
   records. **No DEM is reloaded and no D8 routing is run.**
4. If the cache is absent, stale, incomplete, or fails one of the integrity
   checks, load the source DEM, hierarchy and per-cell runoff, then execute the
   original static `redistributeRunoff()` calculation. Check mass conservation
   and agreement with the scenario's effective runoff volume.
5. Write the result with a Qt `QSaveFile`: the temporary file is committed
   atomically only when all records have been written. Read back the page
   using the normal cache reader and return it.

Requests handled by one `AnalysisService` instance serialize cache validation
and rebuilding with a mutex. This prevents duplicate work on simultaneous
first-use requests in the same process. The cache file uses atomic replacement,
so a process interruption before commit leaves either the previous complete
cache or no cache; it does not publish a half-written result.

**Limitations:** the first query is synchronous, can consume substantial CPU and
RAM, and blocks its HTTP handler until complete. Later requests are bounded by
page size for basin-record reads but still perform input-file metadata checks.
There is not yet an asynchronous redistribution job, progress endpoint, or
cancellation API at the service-query boundary. Independently running server
processes are not coordinated by the in-process mutex; they may redundantly
build the same deterministic derived cache.

## REST response and pagination

```http
GET /v1/jobs/{job-uuid}/rainfall-scenarios/{scenario-uuid}/redistribution?offset=0&limit=50
```

The JSON schema for all physical quantities and basin rows is unchanged.
A new informational Boolean field is included:

- `cache_hit: false` — this request performed a new redistribution calculation
  and committed a fresh cache, whether because it was missing or invalid.
- `cache_hit: true` — the result came from an existing, validated cache.

It is not a physics flag; do not use it to interpret a water level or inundation
area. Both paths return the same deterministic result, given unchanged inputs.
In particular, `offset` beyond `total_basins` returns an empty `basins` array,
`has_more=false`, and the full global totals. Querying `offset=50&limit=50`
reads only that window; it **does not rerun the event**.

### Example sequence

```bash
# Replace the UUID placeholders with values returned by the preceding APIs.
BASE=http://127.0.0.1:8123/v1/jobs/JOB_UUID/rainfall-scenarios/SCENARIO_UUID

# Computes once, creates redistribution-v1.aowbin; normally cache_hit=false.
curl "$BASE/redistribution?offset=0&limit=50"

# Reads the next page from that same result; normally cache_hit=true.
curl "$BASE/redistribution?offset=50&limit=50"

# Restart the Hydrology server, then query again: the file is reused.
curl "$BASE/redistribution?offset=0&limit=50"
```

The in-process API is
`AnalysisService::rainfallRedistribution(jobUuid, scenarioUuid, offset, limit, &error)`;
there is no separate HTTP-only implementation.

## Binary archive contract

The file is versioned and **little-endian**. It uses IEEE-754 binary64 for all
volumes, signed 64-bit parent positions, and unsigned 64-bit indices/flags.
It is meant for **Hydrology's internal derived cache**, not as an interchange
format with Map, GUI, or SWMM. External consumers should use the documented
JSON service API.

| Part | Size | Contents |
|---|---:|---|
| Header | 140 bytes | Magic `AOWRDS01`, u32 format version (=1), u32 columns, u32 rows, u64 basin count, 16-byte job UUID, 16-byte scenario UUID, 16-byte source fingerprint, six f64 balance quantities, u64 boundary flag, u64 FNV-1a checksum |
| Record | 56 bytes each | u64 basin index, i64 parent index (-1 for root), three f64 (`direct_inflow_m3`, `stored_in_subtree_m3`, `overflow_m3`), u64 at-spill flag (0/1), u64 FNV-1a checksum |

The six header quantities, in order, are generated runoff, total root storage,
direct boundary outflow, basin boundary outflow, total boundary outflow and
signed mass-balance residual. Files must have length exactly
`140 + 56 * basin_count` bytes and at most twice as many basins as DEM cells.
Each record's index must equal its position in the archive. Basin parent indices
must point to a later row and remain in bounds, except for the root sentinel.
This preserves deterministic UUID reconstruction using the saved job ID and
internal basin index, without rebuilding the hierarchy for a cached page.

Checksums use **64-bit FNV-1a**, covering each header/record except its trailing
checksum field. These checks detect accidental corruption; **they are not
cryptographic signatures or security controls**. Header validation checks
structure and the volume balance. Page reads check every record returned.
Unrequested records are **not** scanned, because that would defeat the purpose
of bounded pagination. A damaged record will be detected when its page is read.

## Cache invalidation and recovery

The 16-byte source fingerprint is derived from SHA-256 over the file sizes and
last-modified timestamps of `elevation.f32`, `basins.aowbin`, `runoff.f64` and
`scenario.json`. This is intentionally inexpensive: the original source files
can be much larger than the cache and rehashing every byte on every request
would make pagination slow.

Hydrology never modifies those four files after publishing a completed analysis
or scenario. External edits or file replacements are unsupported. Ordinary
changes to their sizes or timestamps invalidate the cache automatically.
A deliberately modified file that preserves *both* its previous size and
modification timestamp is not guaranteed to be detected. Strong, end-to-end
source-content hashing and multi-process locking remain future hardening work.

A cache is **safe to delete** manually while the server is stopped: the next
request reconstructs it from the source files. Never delete the original DEM,
`basins.aowbin`, `runoff.f64` or `scenario.json` expecting automatic restoration.
If these authoritative inputs are absent or corrupt, the query reports an
error. A broken derived cache is automatically regenerated; a broken source
is never silently replaced with guessed values.

## Memory, performance and accuracy

The first calculation uses the original dry D8 graph and the saved depression
hierarchy; it remains in-memory, not out-of-core. The archive saves *subsequent*
queries and survives server restart, but does not make the initial solve
asynchronous or bound its peak RAM consumption. Record writes are buffered,
and subsequent pages read only their own records after validating a fixed-size
header. File size is approximately **140 + 56 × basin count bytes**.

The cached result is the **same static volume-only approximation** as the
uncached result; it does not improve physical accuracy. It contains no
per-cell water depths, transient flow velocities, travel times, flood-wave
propagation, culverts, sewer capacity, or reliable inundation polygons. The
volume conservation invariant remains:

`generated_runoff_m3 = basin_storage_m3 + total_boundary_outflow_m3 + mass_balance_error_m3`

Nested-basin volumes must not be summed across all records. The service's
`basin_storage_m3` field already counts root storage only.

## Tests and operational checks

`tests/test_redistribution.cpp` covers the archive's round-trip format,
version/checksum validation, parent indices and record corruption.
`tests/test_service.cpp` covers first-use creation, pagination, reuse on the
next request, recovery from a corrupt cache and reuse after a server restart.
Run:

```bash
./compile_linux_tests.sh --both
```

For operations, start with a small synthetic or real complete DEM. Verify that
`cache_hit` changes from false to true for consecutive reads, then restart the
server and confirm it stays true. Compare global volume balance and selected
basin IDs before and after restart. Keep the source file timestamps stable.
On large datasets benchmark **first query**, **cached first page**, and
**cached later page** separately; those measure very different work.
