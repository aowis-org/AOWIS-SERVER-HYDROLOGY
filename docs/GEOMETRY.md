# Terrain raster geometry and vertical reference

## Horizontal coordinate system

`RasterGeometry` describes the cell-edge geometry of a north-up raster. Its `origin_x`, `origin_y`, `pixel_width` and `pixel_height` are always **outer cell edges**, with `pixel_width > 0` eastward and `pixel_height > 0` southward. Elevation samples reside at pixel centres. A `5×5` raster therefore has its first sample at `origin_x + 0.5 * pixel_width` and `origin_y - 0.5 * pixel_height`.

Supported by the current core:

- `Wgs84Geographic` — degrees longitude/latitude; EPSG:4326; UTM zone must be zero. Copernicus GLO-30 analyses should stay in this grid to avoid artificial changes from interpolation.
- `Wgs84Utm` — projected WGS84 UTM coordinates, metres, zone `1..60` plus northern/southern hemisphere. Equivalent EPSG codes: 32600 + zone in the north, 32700 + zone in the south. This is a foundation for later projected LiDAR datasets. GeographicLib 2.7 performs the WGS84 ↔ UTM conversion. UPS and arbitrary local/projected CRS are not supported yet.

The current core does not support rotated/sheared GeoTIFF transforms, antimeridian-crossing geographic rasters, rasters beyond the supported latitude window, or other geographic datums. Reject unsupported input rather than silently distort it. Native GeographicLib coordinates can be converted explicitly using `wgs84ToUtm` and `utmToWgs84`.

## Distances and areas

The core now calculates geographic D8 edge lengths using GeographicLib's **WGS84 ellipsoidal geodesics**. Geographic raster cell area is calculated from a WGS84 geodesic polygon bounded by the four cell corners. Because geographic cells are north-up rectangles, per-row metrics can be reused across columns without changing the result. This replaces the earlier spherical Earth radius and planar cosine approximations.

For a north-up UTM raster, physical edge lengths and area are approximated as projected lengths divided by the **local GeographicLib projection scale**, and projected area divided by scale squared. The current implementation evaluates scale once per row at a representative longitude, which is appropriate for small-to-moderate UTM grids but may create small biases across wide projected rasters. Future large/high-accuracy LiDAR analysis may require per-cell scale evaluation or native ellipsoidal polygon/edge calculations.

## Vertical reference

`VerticalDatum` is a distinct metadata field (`Unknown`, `Egm2008`, `EllipsoidalWgs84`). Copernicus GLO-30 uses **EGM2008**; this is not a horizontal CRS. Coordinate conversions do not change elevation values. It would be incorrect to combine layers with different vertical reference systems without an explicitly configured vertical-datum conversion and appropriate geoid data.

`Unknown` must not be interpreted as a verified match. A downstream multi-source DEM merger must validate vertical references before creating a combined analysis. The existing REST Map terrain interface is specified for a single source dataset with known source metadata; no Map API modifications were made here.

## Extents and downstream behavior

Routing stays in the DEM's native grid, avoiding resampling. The grid boundary is a potential outlet; cropped analyses lack upstream or downstream context outside their bounds. The service must disclose that limitation; it is not equivalent to the user's selected rainfall polygon or desired downstream trace length.

The default REST job interface currently accepts geographic WGS84 bounds and requests native elevations from Map. **Only the pure C++ analysis library can accept UTM elevation grids at this stage.** Projected GeoTIFF ingestion and an independent rainfall polygon / processing domain are later roadmap stages.

## Build and submodule integration

`tools/setup_geographiclib.sh` adds/pins GeographicLib 2.7 as a real Git submodule at `external/GeographicLib` inside the Hydrology repository. This cannot be represented by a changed-files-only ZIP alone: Git must create and commit its submodule gitlink. Run the setup script once after applying this patch. It is a conscious, user-invoked dependency setup, never a CMake download.

CMake looks for an existing `GeographicLib::GeographicLib` target before adding a subdirectory, so a parent standalone meta-build can provide an already-built GeographicLib target. If no parent target exists, Hydrology builds against its own submodule source. No GUI, Map or Standalone repository modifications are part of this stage.
