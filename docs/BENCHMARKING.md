# Reproducible Hydrology core benchmarking

## Purpose and limits

This guide provides a **repeatable performance harness**, not a claim that
synthetic elevations reproduce Copernicus GLO-30, nor a validation of predicted
flooding. The opt-in `aowis-hydrology-benchmark` runs actual core algorithms in
one process and reports timing, memory, algorithmic counts, and conservation
checks in **machine-readable JSON**.

The measured workflow is:

1. Generate a north-up, synthetic 30 m UTM 32N raster.
2. Run `analyze()` with the default depression-filling policy (physical
   Priority-Flood, D8 routing, upstream accumulation and fill diagnostics).
3. Release the transient analysis result, retaining the immutable input raster.
4. Build the original-DEM `DepressionHierarchy` with merge saddles/capacities.
5. Generate a uniform, area-aware per-cell effective runoff-volume raster.
6. Call `redistributeRunoff()` using the original DEM, without reusing the
   conditioned D8 elevations, then **verify mass conservation**.

All stages call production C++ core code. Neither Map network acquisition,
GeoTIFF decoding, Stage 7 polygon clipping, SQLite/PostgreSQL, asynchronous
Qt services, HTTP, persistent caches, nor the coupled transient reservoir
solver is included. Measure those independently, using representative inputs.

The *synthetic* vertical datum is unknown. `EPSG:32632` establishes only the
horizontal grid. Raster geometry is 30 m × 30 m in projected coordinates;
GeographicLib is used to correct row-cell areas for the UTM projection scale.

## Building

From `AOWIS-SERVER-HYDROLOGY`:

```bash
cmake -S . -B build-hydrology-benchmark -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAOWIS_HYDROLOGY_BUILD_SERVER=OFF \
  -DAOWIS_HYDROLOGY_BUILD_SERVICE=OFF \
  -DAOWIS_HYDROLOGY_WITH_GDAL=OFF \
  -DAOWIS_HYDROLOGY_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=ON
cmake --build build-hydrology-benchmark --parallel
ctest --test-dir build-hydrology-benchmark -L benchmark-smoke --output-on-failure
```

GeographicLib must already be initialized via `tools/setup_geographiclib.sh`
or supplied with `AOWIS_HYDROLOGY_GEOGRAPHICLIB_SOURCE`. Qt and GDAL are not
required for this benchmark-only build. The ordinary `compile_linux_tests.sh
--both` workflow is unchanged: **benchmarks are disabled by default**. When
enabled with CTest, two small regression checks exercise the report generator
and rejection of inputs exceeding the cell budget; no million-cell test is
added to CI by default.

## Workloads

```bash
# Development baseline (65,536 cells).
./build-hydrology-benchmark/aowis-hydrology-benchmark \
  --columns 256 --rows 256 --terrain rolling --seed 17 > hyd-256.json

# Small district-sized exercise (1,048,576 cells).
./build-hydrology-benchmark/aowis-hydrology-benchmark \
  --columns 1024 --rows 1024 --terrain pitted --seed 17 > hyd-1024.json

# ~3,000 × 3,000 30 m cells, a synthetic ~90 × 90 km domain.
# This can require substantial RAM; run on a dedicated test machine.
./build-hydrology-benchmark/aowis-hydrology-benchmark \
  --columns 3000 --rows 3000 --terrain rolling --seed 17 > hyd-3000.json

# Stress the flat-routing path.
./build-hydrology-benchmark/aowis-hydrology-benchmark \
  --columns 512 --rows 512 --terrain flat --seed 17 > hyd-flat.json
```

Options (all explicit, with no random global state):

| Argument | Default | Supported values |
| --- | --- | --- |
| `--columns`, `--rows` | `256`, `256` | 16–5000 each; maximum **10 million** cells in total |
| `--seed` | `1` | unsigned 32-bit integer |
| `--terrain` | `rolling` | `rolling`, `pitted`, `flat` |
| `--rain-mm` | `35` | 0–1000 mm, uniformly over the full raster |
| `--runoff-fraction` | `0.65` | 0–1 |
| `--help` | — | usage and limits |

`rolling` has a large-scale slope, low-frequency sinusoidal corrugations and
seeded high-frequency noise; it intentionally stresses handling of many
small local minima. `pitted` adds deep rectangular negative-elevation offsets
to that terrain; `flat` is a constant-height plateau. **None is a measured
geographical DEM or a statistical surrogate for a particular city.** The
fixed counter-based noise generator makes a particular raster repeatable on
the same software/platform. Floating-point math and optimization settings can
produce different last bits across architectures.

## Report format and interpretation

Standard output contains exactly one JSON document on success; errors are
written to standard error with nonzero exit status. Each document includes:

| Field | Meaning |
| --- | --- |
| `schema_version` | JSON report schema, currently `1` |
| `benchmark`, `terrain`, `seed`, `cells`, `crs`, `cell_size_m` | Workload identity |
| `elevation_fingerprint_fnv64` | FNV-1a of little-endian *float32 bit patterns*, to detect changed synthetic DEMs; **not** a cryptographic checksum |
| `conditioned_cells` | Number of physically modified fill cells in `analyze()` |
| `basin_count` | Number of leaf and parent depression-hierarchy nodes |
| `generated_runoff_m3`, `retained_m3`, `total_boundary_outflow_m3` | Original-D8 static storm volume accounting |
| `mass_balance_error_m3` | Floating-point residual; failure if outside `1e-7 × max(1, generated_runoff)` |
| `stages[].name`, `stages[].wall_ms` | Per-operation elapsed wall time, **one run**, not a statistical median |
| `stages[].process_rss_bytes` | Total process resident memory after a stage, on Linux; not stage-private allocation |
| `peak_process_rss_bytes` | Linux `/proc/self/status` VmHWM peak RSS of the process; `null` elsewhere |
| `measured_stages_total_ms` | Sum of measured stage wall times, not end-to-end process launch time |

The original DEM remains resident during the workflow. The large
`AnalysisResult` is deliberately discarded before basin construction to keep
the benchmark's peak closer to a streaming pipeline; the service's actual
memory profile may differ. Per-stage RSS includes other allocations and is
not a replacement for heap profiling. Linux VmHWM is a process metric; it
includes GeographicLib and runtime overhead. `null` means unavailable, **not
zero RAM usage**.

A reproducible report requires recording the compiler, compiler flags, build
type, CPU model, core count, system memory, OS, GeographicLib revision,
source Git commit, thread load and benchmark command. Save these outside the
JSON when comparing environments. Run multiple fresh processes (for example,
5–10), compare medians/dispersion, and retain the raw JSON outputs. Do not
interpret a single wall-time difference smaller than operating-system noise
as an optimization.

## Verification and reference comparisons

The benchmark ensures **internal conservation**, not physical correctness.
Separate validation with *actual* GLO-30 data should cover:

1. Source coverage, georeferencing, vertical datum and Map raster delivery.
2. NoData holes, boundaries, internal pits, coastlines, and flat terrain.
3. D8 flow direction, accumulation and catchment boundaries compared against
   an independently configured GIS solver (e.g. GRASS GIS or WhiteboxTools),
   with identical resampling, conditioning, D8 tie rules and catchment limits.
4. Basin volume/level curves checked against direct integration from the
   original elevation raster; basin-merge topology checked on crafted fixtures.
5. Sensitivity to DEM resolution, vertical error, breaching, polygon area,
   runoff fraction, calibrated controls and travel-speed assumptions.
6. Independent HTTP/Map and in-process-service parity on identical terrain
   samples; verify source checksums and scenario provenance across restarts.
7. CPU/RAM and query latencies for district, city and metropolitan extents,
   including acquisition, persistence, repeated cache reads, and concurrent
   requests—not only this isolated core benchmark.

Differences against a reference implementation do not prove an error unless
algorithm, boundary, cell-area and DEM-processing policies are equivalent.
Measurements from real local terrain and independently verified results must
be recorded **separately** from synthetic timings. Document observed errors,
not just successful runs, before using the outputs for engineering decisions.

## Current limits

- Synthetic benchmark only; no actual Copernicus/Map end-to-end timing.
- In-memory core algorithms; the 10-million-cell cap is a safety budget, not
  a guarantee that a particular machine has enough RAM.
- No speedup assertion, throughput SLA, calibration result or metro-sized
  performance claim is implied by the presence of this benchmark.
- Peak memory and stages include allocations from GeographicLib and the C++
  runtime, but omit other services and server-process caches.
- The FNV elevation fingerprint verifies deterministic *input identity* in
  reports, not the integrity of externally delivered DEM files.
