# Rainfall polygons, runoff fractions, and scenario outputs

## Design and ownership

An existing, completed Hydrology terrain-analysis job owns the original DEM,
D8 graph and basin hierarchy. One terrain job can have many independent
**rainfall scenario UUIDs**, each with its own immutable input parameters,
summary and cellwise runoff raster. Reusing the terrain job never requests
another DEM or reruns D8 conditioning. Map remains the sole DEM provider.

`AOWIS::Hydrology` provides the pure C++ `calculateRainfallRunoff()` core.
`AOWIS::HydrologyService` provides `createRainfallScenario()`,
`rainfallScenarios()`, `rainfallScenario()` and `rainfallRunoffWindow()`.
`aowis-server-hydrology` exposes the matching HTTP API; the standalone
application calls the same service directly without a local HTTP server.

## Request and units

Submit a rainfall scenario only after its parent job reaches `completed`:

```http
POST /v1/jobs/{analysis-uuid}/rainfall-scenarios
Content-Type: application/json
```

```json
{
  "name": "User-defined uniform event",
  "geometry": {
    "type": "Polygon",
    "coordinates": [[[8.010,50.010],[8.020,50.010],[8.020,50.020],[8.010,50.020],[8.010,50.010]]]
  },
  "depth_mm": 35,
  "duration_seconds": 3600,
  "effective_runoff_fraction": 0.65
}
```

The `geometry` may also be a WGS84 GeoJSON `MultiPolygon`, with holes.
Every coordinate pair uses longitude, latitude. No date, climate source,
intensity curve or regional preset is assumed. The `depth_mm` field is the
**total uniform depth** across the selected area, not mm/hour; duration
only provides an input-time average, not an arrival time or hydrograph.

The effective runoff fraction must lie in [0,1]. It is the user's assumed
fraction of rain that becomes surface runoff over the event. The retained
remainder is not separately classified into infiltration, evaporation or
storage. It is not a physically calibrated soil-loss model.

The resulting JSON contains an AOWIS scenario UUID, its parent job UUID,
input geometry/parameters, the covered area and wet-cell count, and:

- `gross_rainfall_volume_m3`: depth (metres) × covered DEM area (m²).
- `effective_runoff_volume_m3`: gross rain × effective runoff fraction.
- `retained_or_lost_volume_m3`: gross rain minus effective runoff.
- `average_effective_runoff_m3_s`: effective runoff volume / event duration.
- `touches_domain_boundary`: selected polygon intersects or reaches the DEM
  edge, or extends beyond the analysis window; results may be incomplete.

**The rainfall-scenario output itself does not simulate ponding, pipe drainage,
travel time, velocity, peak discharge or infiltration timing.** Stage 8 adds a
separate on-demand static volume redistribution query using the saved runoff
raster and original-DEM basin hierarchy. See
[RUNOFF_REDISTRIBUTION.md](RUNOFF_REDISTRIBUTION.md). The sum of the saved
runoff cells is the generated runoff volume.

## Geometric coverage and accuracy

Coverage is calculated by clipping each polygon ring to each intersected DEM
cell in raster-grid coordinates. Hole intervals are excluded and overlapping
polygons are treated as a **union**, so overlap never multiplies rainfall.
The calculation integrates linear polygon edges at vertex positions and edge
crossings, rather than merely counting selected cell centres. It multiplies
the covered fraction by the row's physical cell area calculated using
GeographicLib. WGS84 polygons can be projected into the DEM's configured
WGS84 UTM zone for pure-core use.

This is a **cell-scale area approximation**: geographic longitude/latitude
polygons are treated as planar within the pixel, and UTM polygon edges are
projected between vertices. It does not compute geodesic clipping of every
polygon against each DEM pixel. Self-intersecting rings and antimeridian
crossings are unsupported; clients should submit valid, simple GeoJSON
polygons. The raster does not gain resolution beyond the input DEM.

The service rejects scenarios overlapping NoData DEM cells rather than
silently dropping water from the volume balance. Source terrain elevations
are immutable, and runoff volumes are independent of D8 filling/breaching
settings. A scenario whose polygon does not intersect the analyzed terrain
is rejected.

## Retrieval and persistence

```http
GET /v1/jobs/{analysis-uuid}/rainfall-scenarios
GET /v1/jobs/{analysis-uuid}/rainfall-scenarios/{scenario-uuid}
GET /v1/jobs/{analysis-uuid}/rainfall-scenarios/{scenario-uuid}/runoff?x=0&y=0&width=256&height=256
```

The last endpoint returns raw **little-endian float64** values (`m³/cell`),
row-major north-to-south, using the parent analysis's raster geometry; it
has `application/vnd.aowis.raster-f64le` content type. Windows are bounded
by 4096×4096 and at most 1,000,000 cells. Additional per-cell coverage
fractions are persisted as float32 for future analysis and diagnostics.

Each scenario is saved under the parent job's `rainfall-scenarios/{uuid}/`
directory as `scenario.json`, `runoff.f64` and `coverage.f32`. The JSON
manifest is written last through `QSaveFile`; partially written scenarios
are not discoverable through the list/detail APIs. Saved results remain
available after a service restart, subject to the parent analysis job
remaining present. The current service bounds requests to 32 polygons,
30,000 vertices overall, 500,000 raster cells in the selection's bounding
box, and 64 scenario directories per terrain job. Complex clipped cells
have an additional geometric edge budget.

## Future model integration

Stage 8's initial static solver is implemented. It starts from the saved
`runoff.f64` raster and uses **original DEM** dry-D8 paths plus the source-DEM
depression hierarchy to determine retained volumes, spill transfers and
out-of-domain discharge. It does **not** infer timing or a flood-depth map.
Read the [redistribution guide](RUNOFF_REDISTRIBUTION.md) for the endpoint,
accounting formula and limitations. No SWMM execution or precipitation
climatology is included.
