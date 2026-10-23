# Hydrology verification and regression contracts

Hydrology maintains independently testable **numerical**, **service**, and
**HTTP** layers. Tests are organized around the current product's interfaces,
not historical implementation milestones.

## Build and execute

```bash
# Recommended: full service, HTTP, GeoTIFF and core tests (Debug and Release).
./compile_linux_tests.sh --both

# Optional: portable core-only tests when Qt or GDAL are unavailable.
./compile_linux_tests.sh --both --core-only

# To run just HTTP compatibility checks after building the server:
ctest --test-dir build --output-on-failure -R aowis-hydrology-http-contract

# ZIP tooling uses disposable local Git fixtures, including nested submodules.
python3 tests/test_zip_hydrology.py
python3 tests/test_prune_hydrology.py
```

Full-suite validation should use the documented project build scripts on each
supported target. Neither a core-only run nor Python-only tests demonstrate
that the HTTP executable builds or that its interface works.

## HTTP compatibility contract

`tests/test_http_contract.py` starts the actual `aowis-server-hydrology`
executable with a fresh local INI file and isolated output directory. Its
Python-standard-library Map mock returns a small, known, strictly descending
`float32` DEM using Map's analysis-grade grid contract. It requires neither a
running Map server nor a downloaded elevation dataset.

The test exercises:

- `/status`, the job collection, missing resources, invalid JSON, input and
  pagination validation, request-size rejection and associated HTTP codes;
- asynchronous job creation and completion, cell geometry and dataset identity;
- byte-level little-endian raster MIME/layout and known elevation values;
- deterministic flow-related raster products between independent analyses;
- basin listing, drainage network, downstream tracing, synchronous and
  asynchronous catchment masks (byte-identical outputs), transient task cleanup;
- a persisted rainfall scenario and raw `float64` runoff window;
- explicit background redistribution cache creation, repeated byte-equivalent
  cache-only pages, mass-balance consistency and cache reuse after restart;
- discovering and reading jobs, rasters and the scenario after process restart;
- bounded terrain-job admission (429 without disk artifacts), queued-job
  cancellation, immediate slot reuse, and canceled-job persistence on restart.
The separate offline pruning test checks that deletion is opt-in, restricted
to terminal UUID manifests older than the minimum age, and does not traverse
symlinked job directories or remove unrecognized data.

This is a **black-box protocol regression** and must run unchanged against a
replacement HTTP adapter. A change in response status, body, MIME or binary
layout should fail the suite unless it is an explicitly agreed API change.
Random UUIDs, timing measurements and ephemeral ports are deliberately **not**
compared byte-for-byte. The sample DEM fits in one Map patch so its numeric
fixtures do not depend on patch scheduling; multi-patch transport and concurrency
are covered by the separate terrain-transport and benchmark-mock suites.

Each test creates its own temporary data directory and binds to loopback using
available ephemeral ports. It cleans up the server and Map mock even if a test
fails. No fixed `/tmp` name, external network connection or production data is
used. Server launch failures include the server's captured log in the error.

The suite is a *minimum* migration compatibility gate. It does not prove that
all routes or all accepted payloads are compatible; more cases can be added as
needed, without changing the existing baseline expectations.

## Terrain execution-cell budget

In addition to the per-job `max_cells` cap and outstanding queue depth, the
service admits only enough *running* terrain jobs to fit its combined cell
budget. The default `max_active_terrain_cells=0` inherits `max_cells`.
The service test holds one terrain reader while submitting another to verify
that a second worker is not dispatched beyond the cell budget, and that
waiting work starts when running work releases its reservation. Requests
larger than the budget are rejected before any job metadata is created.
CLI tests cover invalid and boundary values; HTTP status exposes the effective
limit and current dispatched-cell count. This is an admission heuristic, not
a measurement of actual RSS or a guarantee for other workload types.

## Independent numerical reference tests

The `numerical-verification` CTest label selects the analytic DEM and
reservoir-routing benchmarks, including a closed-form free-outlet solution
and a separately implemented RK4 coupled-network reference. Test failures
include poor timestep refinement, mass conservation or known terrain
geometry. See [numerical verification](NUMERICAL_VERIFICATION.md) for
assumptions, measured error values, and exact reproducibility commands.

## Numerical and performance baselines

The C++ CTest suite checks pure-core algorithms, conservation cases, DEM
geometry, and service/job persistence (when Qt is available). Numerical
regressions should compare quantities and tolerances supported by analytic or
independent reference cases. Avoid treating hashes of arbitrary platform
floating-point results as scientific ground truth.

For performance comparisons use `docs/BENCHMARKING.md` and record repeatable
settings, machine/build details and observed results in
`docs/BENCHMARK_FINDINGS.md`. Run the same input data and resolution, Map
server conditions, job count, and build configuration on both sides of an
implementation change. Record response percentiles, memory peaks, admitted
work/queue depths, time spent waiting, and Map transfer metrics where relevant.
Do not attribute speedups to the HTTP framework when differences are caused by
DEM cache warmth or numerical work. Scientific correctness is a prerequisite
for performance comparisons, not a performance metric.


## Transient catchment task contract

The HTTP contract suite compares synchronous and asynchronous catchment masks
byte-for-byte, checks invalid outlet requests, task polling, and verifies that
transient catchment task metadata is not restored after restart. For very large
catchments, prefer the async API so the HTTP request worker performs only
submission/polling, rather than traversing the full-grid flow graph. The
immutable downstream raster is memory-mapped and only the outlet's saved
contributing-cell count is read; results do not require decoding two complete
64-bit arrays. Catchment output masks still require one byte per grid cell.

One worker processes catchment tasks at a time to bound peak memory. The task
queue admits four queued/running calculations and retains four completed or
terminal results on disk. The original `GET /catchment` remains synchronous
for existing integrations; replacing that route is not a prerequisite of
HTTP-server migration.

### HTTP transport checks

`aowis-hydrology-http-contract` runs against the real cpp-httplib executable,
including persisted outputs, task APIs and binary MIME types. The executable
loads the pinned vendored `external/cpp-httplib/httplib.h`. Before the initial
build, run `./tools/setup_cpp_httplib.sh`; the resulting header is packaged in
future source ZIPs. The HTTP test also checks early rejection of oversized
request bodies. `aowis-hydrology-server-config-cli` validates worker and
request-queue bounds without opening network sockets. Measure HTTP throughput
against identical workloads; a change of transport alone is not evidence of
a measured speedup.

## Persisted terrain analysis integrity

A completed terrain analysis contains seven row-major primary rasters and one
versioned depression hierarchy archive. The immutable raster files must each
have exactly `columns × rows × bytes_per_cell` bytes. The archive header must
match the job grid, its declared basin count must match its exact byte length,
and its built-in checksum must be valid. Derived rainfall, redistribution,
inundation and coupled-simulation outputs have their own lifecycles and are not
required for the terrain analysis itself to be complete.

New terrain jobs record an `artifact_integrity` inventory in `job.json`, with
SHA-256 and byte length for all eight source artifacts. Hashes are calculated
while writing the primary rasters and archive. The server commits each file
through `QSaveFile` and publishes a `completed` manifest only after all files
have been committed and their structure verified. It does not expose the
completed state to readers until that final manifest has been committed.

At startup, every completed job is checked **before** it is made available as
completed. Files with missing, truncated or mismatching content cause the job
to be marked `failed` with a specific diagnostic and a recommendation to rerun
analysis. This status is persisted; files are not silently deleted or
recomputed. For jobs created before the integrity inventory was introduced,
file lengths, archive header and built-in archive checksum are still validated,
but equal-length modifications to an old raster cannot be detected without a
recorded digest. A present but malformed or partial inventory never silently
falls back to the legacy validation rules.

SHA-256 verification reads the stored artifact contents once per service start.
Large completed repositories can therefore take longer to recover, proportionally
to their stored data size. The hierarchy structure is validated with a streaming
checksum at startup to avoid decoding all basin records into memory; its full
semantic record validation remains performed by the archive decoder when the
hierarchy is read. Tests cover missing files, truncation, equal-length damage,
archive corruption, interrupted publication and legacy inventories.

## Coupled DEM and catchment resource contracts

`server/max_cells` limits terrain-analysis admission; the independently
configurable `server/max_coupled_dem_cells` (default 10 million, range 4 to
100 million) limits the additional DEM read/geometry work required for coupled
internal connections and scenario forcing. Both are exposed by `/status`.
Purely explicit calibrated reservoir simulations do not load terrain and are
not restricted by the coupled DEM budget. Oversized DEM-dependent requests
are rejected before a simulation is enqueued or its directory created.

Numerical-core tests compare the catchment random-access traversal with the
existing in-memory D8 analysis output. The service test checks the mapped
persisted raster against known upstream counts at multiple outlets; the HTTP
contract validates synchronous/async byte-identical masks. Memory mapping
reduces duplicated user-space graph arrays, but page-cache residency and
output-mask memory are still significant at large grid sizes.


## Product acceptance and operational verification

The complete test target includes the numerical reference cases, the C++
service-layer cancellation/admission and persistence tests, the black-box
HTTP contract, executable CLI/configuration tests, synthetic resource
benchmark utilities, the derived-cache benchmark mock, retention-tool
safety and relative documentation links. Execute both Debug and Release:

```bash
./tools/setup_cpp_httplib.sh  # Only needed if the vendored header is absent.
git submodule update --init --recursive  # For ordinary Git clones.
./compile_linux_tests.sh --both
```

`aowis-hydrology-derived-cache-benchmark-tool` uses its own disposable HTTP
fixture and verifies the **measurement workflow**, not live numerical
performance. `aowis-hydrology-documentation-links` checks that local
README/doc references still resolve. Neither test replaces real Map-server
integration, true memory profiling or independent validation against
observed hydrological measurements.

Before releasing or embedding Hydrology in Standalone, also verify the
following with appropriate real workloads:

| Contract | Required evidence |
| --- | --- |
| API and binaries | Real-server HTTP contract passes; same deterministic raster bytes and documented JSON/MIME codes after a restart |
| Embedded operation | Link `AOWIS::HydrologyService` without an HTTP listener, use the host-owned Map adapter, and verify callbacks and lifetime |
| Scientific correctness | Numerical references and mass-balance tests pass; experimental coupled routing remains explicitly labeled |
| Persistence and derived cache | Primary integrity checks reject damaged files; derived caches remain byte-identical on repeated and post-restart reads |
| Resource safety | Confirm 429 admission/queue limits, cancellation/cleanup, realistic concurrent process RSS and disk consumption |
| Performance | Record comparable core, live Map and cached-read measurements with input identities, hardware and source revision |
| Documentation | Examples, API route names, configuration limits and relative documentation links match the built product |

A passed synthetic suite validates the tested contracts; it is not evidence
that large-domain hydraulics are scientifically calibrated, that all machines
have adequate RAM, or that Map/GUI/Standalone have been integrated in an
end-user deployment. Use the [benchmark operating guide](BENCHMARKING.md)
for repeatable workload measurements and [product boundaries](ROADMAP.md)
for the remaining scientific/GUI integration obligations.
