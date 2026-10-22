# Hydraulic spill controls: free-overflow foundation

## Purpose, scope, and deployment status

The free-overflow API described in this document is the **first independently testable hydraulic-rate primitive** for AOWIS basin spill connections. It is an opt-in **Qt-independent C++ API**, available through `AOWIS::Hydrology`, that combines a **specific internal DEM merge junction** with an **externally specified** rectangular broad-crested control. It computes a rate under **free overflow** and can transfer a finite volume over one explicit time step between two externally supplied stage–storage curves.

It **does not change** existing rainfall scenarios, Fill–Spill–Merge redistribution, inundation rasters, REST endpoints, persisted caches, default policies, or their numerical results. **Additional opt-in calibrated tailwater and engineered-opening APIs now exist**; see [Submerged spill controls](SUBMERGED_SPILL_CONTROLS.md). The original free-flow methods and their refusal behavior are unchanged. In particular, it is **not** the fully coupled, time-dependent city-scale flood model, a backwater solver or a replacement for SWMM.

This separation is deliberate. A 30 m DEM provides approximate saddle topology but cannot establish a culvert opening, road overtopping crest width, roughness or discharge coefficient. Adding discharge rates directly to the existing order-based redistribution would create fictitious physical precision.

## Sources of model information

| Quantity | Source | Meaning / constraint |
| --- | --- | --- |
| `junction` | `extractSpillGeometry()` from original DEM/hierarchy | The connecting below-saddle components and formation elevation |
| `first_component_index`, `second_component_index` | `junction.child_contacts[]` | **Exactly two** distinct pre-existing connected components; may be descendants, not immediate hierarchy children |
| Cardinal versus diagonal contacts | Original DEM | Both components **must** have at least one side-sharing raster face; a corner-only contact cannot be treated as an opening |
| `crest_elevation_m` | Independent field-survey / defensible hydraulic interpretation | Crest cannot be below extracted terrain saddle in this primitive; sub-DEM culverts require a separate connection model |
| `effective_width_m` | Survey, culvert/road design or calibrated control section | **Never** inferred from `cardinal_boundary_length_m`, D8 pixel counts or cell size |
| `discharge_factor` | Calibration/engineering assumption | Dimensionless factor in `(0, 1]` multiplying the **ideal** rectangular critical-flow rate; **not** the unit-bearing coefficient from handbook `Q=C L H^1.5` tables |
| First/second water levels | Hydrodynamic state in the **same vertical datum** | Absolute elevations, in metres; depth/height mixing is not allowed |
| First/second stage–storage curves | Externally supplied, physically supported reservoir data | Strictly increasing level and volume, volume zero at first knot; curves must include the crest and relevant pond stages |

The hydraulic parameters are explicitly required; there are **no invented defaults**. The original raster must be associated with the correct terrain revision and vertical datum, and the caller must independently determine whether a **broad-crested rectangular control** is a sensible representation of the actual barrier.

The cardinal-contact check is only a **topological gate**, not evidence of an unobstructed surveyed rectangular weir. Multiway merge junctions, diagonal-only contacts, engineered underpasses and root-basin exits are **not modelled** by this two-component primitive. If a multiway junction has to be resolved, first provide a defensible network of measured control sections rather than arbitrarily pairing its children.

## Free-overflow rating equation

For the ideal, wide, rectangular critical-flow control, critical depth is `2H/3`. The module computes:

\[
Q = C_d \, b \, \sqrt{g} \, \left(\frac{2H}{3}\right)^{3/2}
\]

with:

- `Q`: magnitude of discharge, **m³/s**;
- `C_d`: supplied **dimensionless** `discharge_factor`, `0 < C_d <= 1`;
- `b`: supplied **effective width**, **m**;
- `g = 9.80665 m/s²`;
- `H = max(0, upstream_water_level_m - crest_elevation_m)`, **m**.

The result is **signed**: positive means component 1 → component 2; negative means the reverse. Direction is determined from the **two given water levels**, not child index. A rating with zero head or equal stages returns `NoFlow` with zero rate.

The theoretical SI coefficient `sqrt(g) * (2/3)^1.5` is approximately **1.705 m^0.5/s**. Our `discharge_factor` scales this number. This prevents the common error of entering **1.5** (a dimensional SI handbook coefficient) where a dimensionless factor is expected. A field-calibrated dimensional coefficient `C_SI` would correspond to approximately `C_d = C_SI / 1.705` **for this specific theoretical formulation**, but approach-energy/head definitions still must agree.

**Assumptions:** low approach velocity (upstream water-surface depth approximates specific-energy head), approximately rectangular critical-flow section, steady free overflow during each sampled instant, negligible tailwater effect, and an appropriately calibrated loss factor. The formula is not applicable to orifice/culvert flow or a narrow rough channel without an independently justified approximation.

### Deliberately conservative submergence check in the original free-only API

If the **lower** of the two water levels is **at or above** the crest, and the stages differ, the model returns:

```text
regime = SubmergedUnsupported
signed_discharge_m3_s = NaN
```

This test intentionally rejects some possible free-flow cases (submergence onset depends on critical depth and section details). It does **not** estimate a smaller submerged-flow rate, silently return zero, or treat a high tailwater as a calibrated weir. For explicitly calibrated control sections, use the new `rateCalibratedBroadCrestedSpill()` and `stepCalibratedBroadCrestedSpill()` methods described in [Submerged spill controls](SUBMERGED_SPILL_CONTROLS.md). They require externally justified tailwater rating knots; the original functions intentionally remain free-only. Equal water levels produce zero **net** flow, even when both are above the crest.

## One-step volume transfer

`stepBroadCrestedSpill()` accepts both reservoir stage–storage curves, initial volumes and a positive duration in seconds. The procedure is:

1. Validate geometry/control and **both** storage curves, including their coverage of the crest.
2. Invert each piecewise-linear stage–storage curve to get the two initial water levels.
3. Compute the initial free-overflow rating `Q`.
4. If submerged, return `calculated=false`, **unchanged volumes**, and `NaN` discharge; no redistribution is published.
5. Otherwise request `abs(Q) * duration_seconds` cubic metres (forward Euler).
6. Cap the transfer to available **donor volume above the crest** and **recipient capacity below the crest**. These caps avoid advancing into an unsupported flow regime; they do **not** describe submerged conveyance.
7. Subtract exactly that same transferred quantity from the donor and add it to the receiver. Set the `limited_by_*` flags when a cap was reached.

For volumes `V1` and `V2`, the method maintains `V1_after + V2_after = V1_before + V2_before`, up to floating-point rounding. There is no runoff source or domain sink inside this primitive. A non-submerged step with no head returns `calculated=true`, `NoFlow`, and unchanged volumes.

**Important:** This is **one forward-Euler step**, not a multi-reservoir dynamic solver. It has first-order time-discretization error, cannot cross a crest into backwater/submerged flow, and does not handle rainfall/evaporation within the step, travel time, momentum, channel-network conveyance, tailwater structures, or multiple concurrent controls. Large time steps may be physically inaccurate even when mass balance is exact. Users should not mistake capped volume transfer for a complete flood routing calculation.

### Worked example

For two components with a measured/control crest at 10 m, `b=2 m`, `C_d=0.9`, and levels `12 m` / `8 m` (all relative to the same datum):

```text
H = 12 - 10 = 2 m
Q = 0.9 * 2 * sqrt(9.80665) * (2 * 2 / 3)^1.5
  = 8.67842566 m3/s  (positive: first -> second)
```

If each independently supplied stage–storage curve is `V = 100 m² × water_level`, with initial volumes `1200 m³` and `800 m³`, one **1 s** step requests about `8.6784 m³` and returns:

```text
first_storage_after_m3  = 1191.32157434
second_storage_after_m3 =  808.67842566
sum                      = 2000.00000000 m3
```

A much longer step would be **capped** at the donor/recipient crest storage of `1000 m³` each. When the second surface reaches the crest, subsequent flow requires a submerged-control model; this original method **does not guess** the rest of the hydrograph. A separate opt-in calibrated model is documented in [Submerged spill controls](SUBMERGED_SPILL_CONTROLS.md).

## C++ integration example

```cpp
#include <aowis/hydrology/spill_hydraulics.h>

using namespace Aowis::Hydrology;

const DepressionHierarchy hierarchy = buildDepressionHierarchy(original_dem);
const SpillGeometry geometry = extractSpillGeometry(original_dem, hierarchy);

// Select a verified TWO-contact junction with cardinal connections.
const BasinSpillJunction &junction = geometry.junctions.at(junction_number);
BroadCrestedSpillControl control;
control.first_component_index = junction.child_contacts.at(0).child_basin_index;
control.second_component_index = junction.child_contacts.at(1).child_basin_index;
control.crest_elevation_m = measured_crest_m;
control.effective_width_m = measured_active_width_m;
control.discharge_factor = calibrated_dimensionless_factor;

const SpillDischarge rating = rateBroadCrestedSpill(
    junction, control, first_water_level_m, second_water_level_m);
if (rating.regime == SpillFlowRegime::FreeOverflow)
{
    // The rate is signed; do not turn this into a flow forecast without
    // time-dependent reservoir inflows, outlet networks and calibration.
}
else if (rating.regime == SpillFlowRegime::SubmergedUnsupported)
{
    // No trustworthy rate available from this model.
}

// Supplying valid stage-storage curves is the caller's responsibility.
const SpillTransferStep step = stepBroadCrestedSpill(
    junction, control, curve_first, curve_second,
    volume_first_m3, volume_second_m3, time_step_s);
if (!step.calculated)
{
    // Preserve previous state and select an appropriate hydraulic model.
}
```

Neither API mutates the terrain, hierarchy, curves or persistent scenario data. There is no implicit map-cache access. These functions may be called by **standalone C++ consumers**, but they are **not currently exposed** by `AOWIS::HydrologyService` or the HTTP server.

## Error handling and model availability

| Input/condition | Result |
| --- | --- |
| No head / below crest / equal water surfaces | `NoFlow`, zero discharge; step succeeds with no transfer |
| Free head with lower tailwater below crest | `FreeOverflow`, signed finite rate; step limited by physical volume caps |
| Unequal stages, both at/above crest | `SubmergedUnsupported`, `NaN` rate; step `calculated=false`, storage unchanged |
| Three-or-more-child saddle, diagonal-only contact, mismatched component IDs | `std::invalid_argument` |
| Invalid crest, nonpositive width, factor outside `(0,1]`, non-finite stage | `std::invalid_argument` |
| Missing stage–storage knots, nonmonotonic curve, crest/volume outside supplied range | `std::invalid_argument` |
| Zero/negative/nonfinite duration or overflowing numeric rate/step | `std::invalid_argument` |

The one-step method is **not** intended to catch unexpected exceptions and substitute another allocation policy. Callers should explicitly report the unsupported/invalid state.

## Validation, accuracy and next steps

Run `./compile_linux_tests.sh --both`. The new `aowis-hydrology-spill-hydraulics` test verifies the hand-computed 1.705 coefficient, both flow directions, no flow, submerged refusals, explicit donor/recipient limits, numerical conservation over 100 steps, incorrect control widths, bad calibration and nonmonotone curves, and refusal of diagonal/multiway saddle connectivity. The existing ten Hydrology tests remain unchanged.

Before introducing a time-dependent production solver, we should:

1. Define surveyed/terrain-supported effective cross-sections (including culverts and roadside drainage), datum transformations and data provenance; add higher-resolution local DEM support where available.
2. **Available as separate opt-in primitives:** calibrated weir submergence and fully submerged rectangular engineered-orifice rating. **Still pending:** partial-opening transitions, frictional conduits, backwater and integration of hydraulic controls with persisted project models. See [SUBMERGED_SPILL_CONTROLS.md](SUBMERGED_SPILL_CONTROLS.md).
3. Couple multiple controls, per-basin stage–storage curves, rainfall hyetographs and external outfalls through an adaptive or stability-checked finite-volume time integrator.
4. Validate against laboratory/reference rating curves and observed hydrographs, then benchmark full municipal networks and cache versioning before changing the production redistribution policy.

### Reference literature (equation and limitations)

- [US Bureau of Reclamation, Water Measurement Manual — broad-crested rectangular weirs](https://www.usbr.gov/tsc/techreferences/mands/wmm/chap02_13.html). Derives critical-flow relationships and discusses discharge correction factors.
- [US Bureau of Reclamation — critical flow and submergence](https://www.usbr.gov/tsc/techreferences/mands/wmm/chap02_12.html). Explains why downstream stage matters once the control is submerged.
- [US Army Corps of Engineers, HEC-RAS — weir flow computations](https://www.hec.usace.army.mil/confluence/rasdocs/ras1dtechref/6.5/modeling-bridges/hydraulic-computations-through-the-bridge/high-flow-computations). Presents `Q=C L H^1.5` and emphasizes control geometry, coefficient selection and the upstream energy head.

The links explain the underlying hydraulics; **they do not validate** the coefficients, raster saddle geometry or cross sections for any particular AOWIS dataset.
