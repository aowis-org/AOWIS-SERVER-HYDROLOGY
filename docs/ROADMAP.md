# Hydrology product scope and open engineering questions

This document describes the **current product boundary** and unresolved
engineering/scientific questions. It is not a record of patch versions,
implementation increments or completed development tasks. The [README](../README.md)
indexes detailed feature documentation; [Integration](INTEGRATION.md) describes
runtime ownership and [Benchmark findings](BENCHMARK_FINDINGS.md) preserves
measured performance evidence.

## Available product subsystems

| Subsystem | Current scope | Important constraint |
| --- | --- | --- |
| DEM conditioning and D8 drainage | Flow directions, contributing area, catchments, channel and watershed queries, optional bounded local breaching | Cropped DEM boundaries are not proven real-world outlets; raster quality limits results |
| Original-DEM depression storage | Hierarchy, saddles, storage capacities, curve queries, persistent UUID-linked basins | Raster-centre bathtub assumptions; no hydraulic timing |
| Rainfall scenarios | Polygon-clipped event totals, assumed effective-runoff fraction, persisted per-cell runoff volume | No infiltration time series or calibrated hydrograph automatically derived |
| Static redistribution and inundation | Volume-conserving basin allocation and conditional ponded-depth rasters | Heuristic sibling allocation and potentially unavailable depth under unresolved partial merges |
| Coupled reservoir simulation | Explicit storage curves, hydraulic-control ratings, inflow histories, time stepping and outlet accounting | Experimental; geometry and coefficients must be sourced externally |
| Scenario-driven coupled forcing | Explicit temporal fractions; optional constant-velocity dry-D8 travel | Not calibrated flow attenuation or a distributed 2D flow solver |
| Transport and persistence | REST and C++ library, asynchronous tasks, bounded reads, integrity-checked caches | Analysis-worker concurrency increases peak RAM; persistence requires disk budget |

Hydrology's current analytical outputs are **screening and scenario tools**.
They cannot substitute for surveyed drainage structures, measured rainfall and
hydraulic calibration, or independent flood-hazard validation.

## External responsibilities

- **Map** supplies DEM samples and owns dataset acquisition, enabled datasets,
  download/cache policy and source metadata. Hydrology does not fetch raw DEM
  downloads, treat rendered tiles as ground truth or operate another Map cache.
- **GUI** supplies geographic interaction, scenario input, visualization and
  error/uncertainty display. A complete end-user map workflow requires work
  in that separate repository.
- **Standalone** embeds the same Hydrology service and borrows the application's
  already-initialized Map object; no loopback HTTP server is required.
- **SWMM** is a separate future network solver/integration concern. Hydrology
  currently does not solve SWMM storm-drainage networks.

## Open scientific and functional questions

1. **Reference accuracy:** compare D8 routes, depressions, spill levels,
   storage curves and runoff partitioning with independent GIS methods and
   reference DEMs; compare inundation predictions against observed events.
2. **Terrain/domain uncertainty:** characterize source-DEM resolution and
   vertical-datum effects, artificial crop boundaries, NoData handling,
   building/vegetation artifacts, local obstruction and breaching assumptions.
3. **Physical runoff and coupling:** support measured space/time rainfall,
   infiltration and roughness, surveyed inlet/culvert/pump geometry,
   calibrated controls, variable travel times, downstream stages and backwater
   where scientifically justified.
4. **Connectivity sensitivity:** replace or calibrate heuristic shared-saddle
   storage allocations using independently supported hydraulic information;
   retain clear `allocation_assumed`/unavailable-result signals.
5. **End-to-end client integration:** provide a coherent GUI selection →
   analysis → scenario → results workflow, with memory/resource safeguards and
   visibly stated model assumptions.

## Performance questions worth reopening

The present single-host, same-area tests indicate that optimized hierarchy
construction, bounded Map fetching and multi-job execution work adequately for
the measured domains. [Benchmark findings](BENCHMARK_FINDINGS.md) documents the
numbers and why no global Map-request limiter is currently justified.

Revisit profiling when real user workflows show unacceptable delay, when
larger regions exceed memory/disk limits, or when CPU/transport instrumentation
isolates a meaningful bottleneck. Useful next measurements include different
geographic areas, separate (rather than identical) concurrent jobs, true
cold/warm Map cache runs, process/host CPU frequency and load, persistence
scaling, and longer-lived RSS/heap observations. Do **not** interpret one
microbenchmark as a universal configuration recommendation.

## Testing and acceptance principles

For every substantial algorithm or interface change, test conservation and
geospatial invariants separately from speed. Preserve deterministic fixtures,
source-DEM fingerprints, exact window parity, immutable original elevations,
output/archive compatibility and cancellation/restart behavior. Compare
wall-time distributions and memory on equivalent workloads rather than
quoting isolated best runs. Regression suites and reproducible benchmark
commands are in [Benchmarking](BENCHMARKING.md).
