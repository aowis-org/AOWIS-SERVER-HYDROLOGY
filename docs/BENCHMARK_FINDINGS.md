# Hydrology performance findings and decision record

This document preserves the **measured behavior of AOWIS Hydrology** so that
performance work can resume later without reconstructing an implementation
history. It intentionally records experiments and outcomes, **not patch
versions**. To reproduce the experiments, use [BENCHMARKING.md](BENCHMARKING.md).
The [README](../README.md) describes the present product and the
[scope document](ROADMAP.md) describes outstanding questions.

**Evidence status:** local developer-machine measurements in October 2026,
not a controlled hardware performance certification. The supplied logs
identify Linux and GNU 16.2.1 for the synthetic benchmark build, and a
Copernicus GLO-30 dataset sampled by the local Map service for the live
workloads. CPU clocks, process affinity, full cache state and complete host
load measurements were **not** archived; results should not be extrapolated
to different computers, DEM distributions or deployed server fleets.

## Conclusions to preserve

1. **Optimize the hierarchy before micro-tuning transport.** On the measured
   195,712-cell live region, row-wise GeographicLib cell-area reuse reduced
   hierarchy construction from 688 to 146 ms and worker runtime from 1,038
   to 530 ms (two matched single-job observations). This was the largest
   clearly observed local optimization.
2. **Parallel terrain acquisition is useful.** Multiple outstanding Map
   HTTP patch requests reduce per-job terrain-fetch wall time on the tested
   region. This does **not** imply unbounded requests are desirable.
3. **Several analysis jobs can make progress concurrently.** Four configured
   workers increased batch throughput in the small-region test, with higher
   concurrent memory requirements. Workload and server conditions matter.
4. **The per-job Map HTTP cap is not a proven global bottleneck.** The
   three-round large-region comparisons of 2, 3 and 4 requests have close
   four-job throughput medians. Preserve the configurable default of 4;
   there is insufficient evidence for a global Map-request limiter.
5. **The observed delayed RSS plateaus are not a leak test.** Process RSS
   rises during multi-job work and often drops after completion. We have
   bounded sampled results for these domains, not a proof of leak freedom
   or maximum memory requirements on larger regions.
6. **Input parity is a prerequisite for any speed claim.** Identical saved
   elevation-window SHA-256 and stable basin/conditioning counts were
   checked on the measured Map-sourced regions. Full-grid or real-world
   hydrological accuracy has not been established by these comparisons.

## Workloads and reference identities

| Workload | Domain and geometry | Dataset | Recorded identity |
| --- | --- | --- | --- |
| Synthetic pitted grid | 1024 × 1024 = 1,048,576 cells; 30 m UTM zone 32N; seed 17; 35 mm rainfall, 0.65 runoff fraction | Generated terrain | FNV64 `28c391d17bdd1d86` |
| Small Map window | 47 × 75 = 3,525 cells; 30 m | Copernicus GLO-30 / EGM2008 | SHA-256 `e9db2b1f48406446f04fcb0ec63c77b16620f5acd2b53d9206a3b0e04bc98da2` (saved sample) |
| District Map window | 235 × 371 = 87,185 cells; 30 m | Copernicus GLO-30 / EGM2008 | SHA-256 `a385ac127433a5ef0fc2f3edb5272094852dc814cddf1dd2119c18879b0f6b55` (saved sample) |
| Regional Map window | 352 × 556 = 195,712 cells; 30 m; WGS84 bounds 8.600–8.750 E, 50.650–50.800 N | Copernicus GLO-30 / EGM2008 | 4,487 basins; 14,757 conditioned cells; SHA-256 `8f2a9d390b1f7b9596fada675d5d15595c8b3233339ca8576c327cf5709151a3` (saved sample) |
| Large Map window | 775 × 1224 = 948,600 cells; 30 m; WGS84 bounds 8.510–8.840 E, 50.560–50.890 N | Copernicus GLO-30 / EGM2008 | 21,305 basins; 74,246 conditioned cells; SHA-256 `c643c67eddb479335f45f77a93864e41f233da4dc8e8635d90e5c3459fc66d8e` (saved sample) |

For Map jobs, the SHA-256 fingerprints above refer to the benchmark's saved
**elevation window**, not every original DEM cell. Exact byte-for-byte Map
versus Hydrology parity and 0 m maximum difference were observed on the
checked samples. The Map parity query must be aligned with the full source
patch grid before cropping, to avoid false pixel-window mismatches.

## Core benchmark: an actual measured baseline

The original one-process synthetic pitted run (1,048,576 cells, seed 17)
recorded:

| Stage | Wall time |
| --- | ---: |
| Synthetic terrain creation | 28.02 ms |
| D8 conditioning | 532.54 ms |
| Depression hierarchy | **1,583.03 ms** |
| Effective-runoff raster | 1.24 ms |
| Static redistribution | 428.48 ms |
| Sum of measured stages | **2,573.30 ms** |
| Linux process peak RSS | **114.6 MiB** |

It built 179,581 hierarchy nodes, generated 21,486,146.72 m³ effective
runoff, retained 21,329,264.11 m³ and reported 156,882.60 m³ total boundary
outflow; numerical mass-balance residual was approximately
`−2.51 × 10⁻⁵ m³`. These synthetic volumes are a conservation check, not
real precipitation or predicted flooding.

In later controlled core comparisons using row-area reuse, the same 1024²
pitted workload's hierarchy stage changed from approximately **1,273 to
653 ms** (about 1.95× faster). Five paired 512² synthetic seeds showed a
median hierarchy speedup of approximately **2.08×**, with matched basin and
runoff accounting. These are measurements of an implementation change on the
developer host, not an invariant expected on every build. The earlier
1,583 ms sample used a different run context and should **not** be used as
the numerator of the 1,273→653 ms controlled comparison.

## Real Map-sourced hierarchy improvement

The same 352 × 556 source region was measured before and after the
row-area optimization. The local Release server reported:

| Server-side operation | Before | After | Interpretation |
| --- | ---: | ---: | --- |
| Terrain fetch | 222.69 ms | 225.26 ms | Approximately unchanged |
| D8 analysis | 92.22 ms | 112.00 ms | Single-run variation; no regression conclusion |
| Depression hierarchy | **688.21 ms** | **146.06 ms** | **78.8% shorter** |
| Entire worker | **1,037.69 ms** | **530.12 ms** | **48.9% shorter** |

The source elevation sample hash, 4,487 basin count and 14,757 conditioned
cells remained identical. This is a strong **local** speedup observation, but
only one matched run of each version; physical storage-curve validation is
covered by separate algorithm tests, not by these timings.

A separate 948,600-cell run found hierarchy around **1,122 → 664 ms** and
entire worker around **2,256 → 1,648 ms** after a singleton-height/scratch
allocation optimization, with unchanged basin counts and checked DEM hash.
Again, this is one run per implementation and not a proven global speedup.

## Map REST transport observations

- Per-worker reuse of a Qt network manager did **not** demonstrate a speedup
  in the 195,712-cell paired observation: terrain fetch was 225.26 ms before
  versus 288.34 ms after; entire worker 530.12 versus 559.58 ms. Preserve
  connection reuse as a sensible lifetime policy, **not** as a proven win.
- Map-patch instrumentation separated request wall time, summed overlapping
  request durations, bytes/patch counts and raster assembly. In repeated
  measurements, Map requests overwhelmingly dominated terrain-fetch wall
  time; the observed raster copying component was tiny (roughly 0.13 ms in
  an earlier regional series). `map_request_ms` adds overlapping request
  intervals and can exceed actual elapsed terrain-fetch time.
- A read-only replay of the same source patch layout saw about **1.88–2.12×**
  improvement with four outstanding Map GETs compared with one in three
  local runs, with byte-identical patches. This was transport-only evidence.
- When bounded concurrent patch fetching was used by production Hydrology,
  three five-job series on the 195,712-cell region reported approximately
  **269 → 128–130 ms** median terrain fetch and **500 → 341–347 ms** median
  worker duration, without changing sampled elevation hashes or basin counts.
  This measurement does not prove the improvement for all Map datasets.

## Parallel analyses and Map-request settings

On the 195,712-cell region, three-wave medians when changing the **analysis
worker pool** were approximately **2.22, 4.00 and 5.01 completed jobs/s** for
one, two and four workers respectively, under the measured local load. This
shows actual inter-job overlap; it is **not** sustainable system throughput.

The larger 948,600-cell experiments used **four analysis workers** throughout
and varied *only* `terrain/max_concurrent_requests` among 2, 3 and 4. Each
configuration submitted three single-job waves and three four-job waves (15
persisted jobs/configuration). Map-patch size was 256; maximum possible
in-flight patch requests were 8, 12 and 16 respectively. All runs retained
the same checked elevation sample, basin count and conditioned-cell count.

### Comparable three-round medians

| Measurement | 2 Map requests/job | 3 Map requests/job | 4 Map requests/job |
| --- | ---: | ---: | ---: |
| One-job completion wall time | **1,695 ms** | 1,961 ms | 1,754 ms |
| One-job terrain fetch | 586 ms | 502 ms | **448 ms** |
| Four-job batch wall time | 4,328 ms | 4,145 ms | **4,110 ms** |
| Four-job batch throughput | 0.924 jobs/s | 0.965 jobs/s | **0.973 jobs/s** |
| Four-job median per-job terrain fetch | 1,687 ms | 1,527 ms | 1,529 ms |
| Four-job median per-job D8 analysis | 1,079 ms | 944 ms | 951 ms |
| Four-job median per-job hierarchy | 1,256 ms | 1,274 ms | 1,275 ms |
| Four-job median peak Hydrology RSS | 440.8 MiB | 439.9 MiB | 441.3 MiB |
| Four-job median settled Hydrology RSS | 262.4 MiB | 262.6 MiB | 248.6 MiB |

**Individual four-job wave throughputs (jobs/s)**, retained to show the
variation hidden by medians:

| Wave within configuration | 2 requests | 3 requests | 4 requests |
| --- | ---: | ---: | ---: |
| 1 | 0.792 | 0.918 | 0.936 |
| 2 | 0.924 | 0.965 | 0.973 |
| 3 | 1.086 | 1.213 | 1.088 |

The median difference between 3 and 4 requests is **less than 1%**; the
median difference between 2 and 4 is about **5.3%**. Both are small relative
to wave-to-wave variation and differences in non-transport D8/hierarchy
execution. The runs were not interleaved across the three configurations;
cache, CPU clock/scheduling, background load and process lifetimes were not
held constant. **There is no statistically defensible global winner.** The
current default of four patch requests/job is reasonable; the data do not
support adding a shared Map request semaphore or claiming that 16 requests
are inherently harmful.

### Memory interpretation

At four large-region jobs, sampled Hydrology RSS was consistently around
**440 MiB peak**. Delayed RSS was typically around **249–263 MiB**, with
individual process lifetime/baseline differences. RSS can reflect libc
allocator arenas, cached memory and still-live objects; a five-second
post-wave observation cannot distinguish these. Map has its own, generally
larger, background cache footprint. This is not a long-duration leak test,
not maximum aggregate host RAM and not a safe memory budget for city-size jobs.

## What we have *not* established

- Whole-grid identity from a sampled window, or predictive accuracy versus
  measured rainfall, flow, flood marks, surveyed culverts or reference solvers.
- A universally optimal request cap, worker count, patch size or Map cache
  policy for geographic areas other than the tested ones.
- Sustainable multi-user admission throughput, cold source-download behavior,
  metropolitan-scale RAM limits, out-of-core requirements, or absence of leaks.
- A quantitative benefit from cross-analysis in-flight request coalescing.
  Each large four-job wave requests the **same** 20 Map patches four times;
  duplicated work could be reduced only after checking Map's existing cache
  and request behavior. Do not build another uncontrolled DEM cache by default.

## When and how to resume performance work

Resume only for a measured problem: unacceptably slow interactive workflows,
excessive memory on larger domains, server contention with other workloads,
or a reproducible regression after an algorithm change.

1. **Lock result identity first:** save dataset, vertical datum, domain,
   dimensions, resolution, source-DEM window SHA-256, basin/conditioned counts
   and mass-balance checks. Keep the Map source patch geometry fixed.
2. **Record the environment:** exact binary commit, compiler/build type,
   GeographicLib revision, CPU frequency/load, available RAM, Map settings,
   Hydrology INI, cache state and both process PIDs.
3. **Use more representative jobs:** multiple distinct regions and varied
   terrain complexity; repeat with 1, 2 and 4 independent jobs plus an
   explicit memory budget. Never conflate repeated cached results with fresh
   analyses or synthetic core measurements with HTTP service latency.
4. **Instrument the bottleneck, not a guessed optimization:** inspect worker
   `stage_timings_ms`, `terrain_fetch_details`, queue waits, Map CPU/cache
   behavior and transient/settled memory before selecting a code change.
5. **Make paired repeatable comparisons:** interleave configurations, retain
   all wave JSON, compare distributions, confirm identical results, and
   compare with an independent reference model when scientific assumptions
   have changed.

Until a real workflow exposes a bottleneck, further sub-10% transport
micro-tuning has lower expected value than finishing GUI integration and
scientifically validating runoff and inundation assumptions.
