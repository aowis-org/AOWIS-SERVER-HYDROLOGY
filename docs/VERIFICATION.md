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
submission/polling, rather than running full-grid graph reconstruction.

One worker processes catchment tasks at a time to bound peak memory. The task
queue admits four queued/running calculations and retains four completed or
terminal results on disk. The original `GET /catchment` remains synchronous
for existing integrations; replacing that route is not a prerequisite of
HTTP-server migration.
