# Local Copernicus GLO-30 GeoTIFF ingestion

## Purpose and supported data

AOWIS Hydrology can analyze a local directory of Copernicus GLO-30 DEM GeoTIFFs **without** any Map-server changes or an HTTP round-trip. This is a permanent input adapter for controlled local data and benchmarking; Map remains responsible for normal configured terrain acquisition and caching once its analysis-grade source interface is available.

The adapter uses the GDAL C++ raster API and is built as Qt-independent library `AOWIS::HydrologyRaster`, separate from `AOWIS::HydrologyService` and the Qt HTTP adapter. It is read-only: it never downloads, alters, or creates source GeoTIFFs. GDAL is an optional dependency: `AOWIS_HYDROLOGY_WITH_GDAL=OFF` omits this library. The existing Map REST/linked-library adapters and injected `terrain_reader` callbacks remain usable with GDAL disabled.

Initial supported source constraints:

- One-band, north-up, nonrotated GeoTIFF elevations with horizontal CRS **WGS84 / EPSG:4326** and Copernicus **EGM2008** vertical heights. Horizontal EPSG:4326 metadata can be verified in GeoTIFF files, but EGM2008 is an *input dataset assumption*; it cannot always be verified from the TIFF coordinate metadata alone.
- Every overlapping tile must share the same pixel resolution and grid alignment; mismatched files fail explicitly. **Overlapping source tile footprints are rejected** to avoid dependence on filename order. Source files outside the analysis region are not used.
- Recursively reads `.tif`/`.tiff` in the configured directory. If any filename ends in `_DEM.tif` or `_DEM.tiff` (case-insensitive), only those source DEM files are included; Copernicus ancillary layers such as `_EDM.tif` must not be interpreted as elevations. For custom TIFF names, put only elevation rasters in that directory.
- The analysis job supplies one WGS84 rectangular grid; adjacent source DEMs are sampled together through GDAL's mosaic/warp machinery. **Nearest-neighbor** sampling retains original cell elevations instead of inventing interpolated heights. A configured 30 m output grid does not necessarily coincide with native 1-arcsecond pixels; the job response reports the actual grid geometry.
- **Every requested cell must be covered by finite DEM data.** A missing source tile, internal NoData hole, or missing outer boundary value fails the job with the count of missing cells and the first missing grid coordinate. We do not treat missing source coverage as a natural drainage outlet.
- Requested grids are limited by `server/max_cells` (default 10 million); reading and computing are still in-memory. Analysis polygons, extrapolated catchments, hydrologic edge uncertainty and out-of-core processing are future stages.

## Use as a development and test input

Hydrology's production server now always obtains DEM elevations from
AOWIS-SERVER-MAP over HTTP. It no longer accepts `mode=geotiff` or
`terrain/local_dem_directory` in its INI file. Standalone use instead borrows
an existing Map C++ instance. The GDAL reader remains a separately linkable,
Qt-independent **test/development library**, not an alternate production
terrain provider.

Install `libgdal-dev` for `AOWIS_HYDROLOGY_WITH_GDAL=ON` (the default). To omit
this optional library, use `-DAOWIS_HYDROLOGY_WITH_GDAL=OFF`. Programs and unit
tests requiring controlled DEM input can link `AOWIS::HydrologyRaster` and call
`readGeoTiffTerrain(directory, grid, cancellation)`. An `AnalysisService` test
can inject a `ServiceConfig::terrain_reader` callback invoking this function;
the Hydrology HTTP executable itself never accepts local DEM directories.

## Tests and checks

`aowis-hydrology-geotiff` constructs temporary, deterministic WGS84 GeoTIFF fixtures at test time. It tests adjacent tiles, nearest-neighbor sampling, NoData holes, missing edge coverage, cancellation, CRS/datum rejection, Copernicus ancillary TIFF filtering, overlapping files and misaligned source grids. The service tests additionally reject incomplete raster coverage from custom providers.

```bash
./compile_linux_tests.sh --both
# Core/GDAL only, when Qt is unavailable:
./compile_linux_tests.sh --both --core-only
```

For actual Copernicus input, test with a small set of adjacent downloaded `*_DEM.tif` files first and compare sampled elevations and drainage against an independent GIS application. Current synthetic fixture tests establish ingestion behavior but are not a benchmark or validation of the accuracy of Copernicus flood predictions.

## Planned extensions

- Coordinate-aware source raster catalog, reproducible source fingerprints and caching keyed by input files and analysis parameters.
- Projected LiDAR DEMs and arbitrary coordinate transforms, separate from the native GLO-30 path.
- Scalable DEM streaming/out-of-core processing when metropolitan benchmarks show a need.
- Conditioned DEMs and depression-aware drainage analysis; do not mistake GLO-30's visible roofs, tree canopy, embankments or artificial depressions for true ground-level flooding.
