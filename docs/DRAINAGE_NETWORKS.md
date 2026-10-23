# D8 downstream tracing and drainage-network extraction

The reusable C++ core (`include/aowis/hydrology/flow_network.h`) provides
`traceDownstream()` and `extractChannelNetwork()`. Both operate on an existing
D8 flow graph. They do **not** modify terrain, recalculate Priority-Flood,
or download anything from Map.

## Downstream path

`traceDownstream()` starts from a single cell and follows its D8 downstream
pointers, measuring distances along each cell-centre-to-cell-centre link in
metres via the same GeographicLib-aware raster geometry used by analysis.
The path includes cell indices, distance since the start, conditioned
surface elevation and overall path slope (positive for downhill). Stopping
reasons are `domain_boundary`, `internal_sink`, `distance_limit`, and
`point_limit`. The path is limited by complete **link lengths**; the last
cell lies within the specified distance, so the returned traced length may
be less than the user's requested distance. The configured point cap is
10,000 for persisted-job queries.

A domain boundary means the available terrain ends, **not** that water
necessarily terminates there. Tracing through additional terrain requires a
larger analysis domain. For preserved depressions, `internal_sink` indicates
a cell lacking a downstream link. For filled depressions, a physical level
surface may still have an unambiguous D8 route because plateau routing is
represented topologically rather than by artificially altering heights.

The core has a reader-callback overload, allowing the application service
to read only the downstream/elevation entries visited along the path from
persisted binary rasters. This is suitable for metropolitan-sized analyses.

## Channel network

Cells are classified as a *channel* if their upstream contributing area in
square metres is greater than or equal to the specified threshold. The
algorithm finds heads (zero incoming **selected** channel cells),
confluences (two or more), and terminal nodes (missing or non-selected
outflow). Degree-two chains become polyline segments joining these nodes.
A stable segment ID is the index of its upstream node **within that analysis
raster**. The downstream node is recorded explicitly.

Strahler stream order is computed only on the selected channel graph:
headwaters start at order 1; a junction increases the order only if at least
two incoming streams share the greatest incoming order. This differs from
counting every upslope, non-channel D8 cell as a tributary.

Outputs include nodes, channel segments (with georeferenced coordinates),
segment lengths, channel Strahler orders and accumulated upstream areas.
Results are **indicative** with GLO-30 data and should not be treated as
surveyed rivers or culverts. Selected channels at the computation boundary
may have omitted upstream contributions; the current network API explicitly
labels the domain as potentially clipped.

## Library and REST interfaces

The transport-independent service adds:

- `AnalysisService::downstreamTrace(job_id, x, y, distance_km, error)`
- `AnalysisService::channelNetwork(job_id, minimum_area_km2, error)`

REST exposes the same operations:

```http
GET /v1/jobs/{job_id}/trace?x=100&y=100&distance_km=20
GET /v1/jobs/{job_id}/network?min_upstream_area_km2=2
```

`trace` returns a GeoJSON Feature (Point for a zero-length trace or LineString
otherwise), with per-point metadata under `properties.points`. The
`properties.termination` field distinguishes reaching the domain boundary
from a natural sink or configured limit.

`network` returns a GeoJSON FeatureCollection of channel LineStrings, with
node metadata as an additional top-level property. Coordinates are WGS84
longitude/latitude even if the analysis is performed on a UTM raster.

The service limits extracted channels to **25,000 selected cells** and
**2,000 segments** per request; too-small contributing-area thresholds are
rejected rather than creating enormous HTTP responses. Raise the threshold
or use a smaller analysis domain to reduce the requested network. Additional spatial indexing, caching and asynchronous network extraction
may improve repeated large-area requests.

## Tests and limitations

`aowis-hydrology-flow-network` tests downstream path length/limits, domain
versus interior termination, native reader parity, converging tributaries,
Strahler orders, topology, invalid links, cycles, and resource caps.
`aowis-hydrology-service` additionally checks queries before and after
restarting the job service.

The basic flow-graph query itself does not calculate rainfall runoff,
flow arrival times, or basin overflow. Watershed polygon boundaries and
polygon-selected downstream networks are **available as separate spatial
queries** described below. No changes to Map, GUI, Standalone or SWMM are
required for these library and REST queries.

## Watershed polygons and geographically selected drainage

The C++ core additionally provides `polygonizeCatchment()`,
`extractWindowChannelNetwork()`, and `downstreamNetworkFromPolygon()` in
`spatial_queries.h`. The reusable `AnalysisService` exposes these via:

```cpp
QJsonObject watershedBoundary(const QString &job_id, int x, int y,
                              QString *error) const;
QJsonObject channelNetworkWindow(const QString &job_id, double minimum_area_km2,
                                 const DrainageWindow &window, QString *error) const;
QJsonObject affectedNetwork(const QString &job_id, const QJsonObject &request,
                            QString *error) const;
```

Distributed clients call the same service via HTTP:

```http
GET /v1/jobs/{job_id}/watershed?x=100&y=100
GET /v1/jobs/{job_id}/network?min_upstream_area_km2=2&west=8.0&south=50.0&east=8.1&north=50.1
POST /v1/jobs/{job_id}/affected-network
Content-Type: application/json

{
  "geometry": {
    "type": "Polygon",
    "coordinates": [[[8.0,50.0],[8.02,50.0],[8.02,50.02],[8.0,50.02],[8.0,50.0]]]
  },
  "min_upstream_area_km2": 0.1,
  "window": {"west":8.0,"south":49.9,"east":8.2,"north":50.1}
}
```

`geometry` accepts a WGS84 `Polygon` or `MultiPolygon`, including inner
rings (holes). Input coordinate sequences must explicitly close. A polygon
selects **DEM cell centres**; this is a topological D8 influence query, *not*
a spatially integrated rainfall volume. Partial-cell weighting belongs to the separate implemented rainfall-scenario
geometry model; this drainage query deliberately selects cell centres. Values outside the DEM are not fetched by the
query, and incomplete drainage beyond the analysis boundary is reported.

The watershed endpoint returns a GeoJSON `Feature` whose geometry is a
`MultiPolygon` of actual **cell-edge** boundaries, including disjoint pieces
and holes; it does not approximate the boundary from channel centre lines.
Its properties contain `outlet_cell`, `contributing_cells`,
`contributing_area_m2`, `touches_domain_boundary`, and
`boundary_truncation_possible`. The last flag is true when the returned
catchment includes a cell at the computational domain edge: incoming
upstream terrain may be missing. Even an interior result remains sensitive
to DEM quality, artificial conditioning and uncertain surface connectivity.

For `affected-network`, all source cells within the supplied geographic
polygon are followed downstream to a sink or the analysis boundary. The
output is a GeoJSON FeatureCollection of **thresholded channel** portions of
the affected D8 graph, not a claim that water will reach those places during
any particular storm. It reports `source_cells`, `downstream_affected_cells`,
`affected_domain_boundary_cells`, `window_clipped`, and `local_stream_order`.
Only selected cells inside the optional `window` are exported. A window can
exclude streams upstream or downstream of the requested area; stream orders
are recalculated **locally within the visible subset** and must not be
interpreted as full-basin Strahler orders. Contributing areas continue to
use the full previously computed raster, where available.

Ordinary `/network` is unchanged when no `west/south/east/north` query
parameters are provided. Specifying any one of the four requires all four.
The windowed variant applies the same selected-cell and segment budgets
*after* the spatial restriction, allowing useful small-window queries even
if a complete metropolitan network exceeds these caps.

### Query budgets

- Watershed boundaries: at most 100,000 unsimplified raster edges.
- Polygon query: at most 32 polygons, 65 rings per polygon, 2,048 vertices
  per ring and 30,000 input vertices overall.
- Polygon source selection: at most 100,000 covered cell centres.
- Affected downstream cells: at most 250,000 per request.
- Extracted channel network: at most 25,000 selected channel cells and 2,000
  segment features. Longer results fail rather than being truncated silently.
- In-process and HTTP endpoints share the same limits and computations.

Network clipping is a **spatial output filter**, not a bounded computation:
initial processing still reads the stored full-domain D8 links/areas to
retain correct upstream-area values. Spatial indexing, streaming polygon
output, and asynchronous large-area queries are planned performance work.
