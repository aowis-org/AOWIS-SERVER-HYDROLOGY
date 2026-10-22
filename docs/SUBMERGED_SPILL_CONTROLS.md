# Calibrated tailwater and engineered-opening controls

## Scope, implementation status, and what is *not* being simulated

AOWIS's Qt-independent `AOWIS::Hydrology` library contains three distinct hydraulic-control primitives:

| API | Physical situation | Inputs that **must** come from outside the DEM | Can transfer storage in time? |
| --- | --- | --- | --- |
| `rateBroadCrestedSpill()` / `stepBroadCrestedSpill()` | Rectangular **free** overflow over a crest | Effective width, crest and dimensionless factor, stage–storage curves | Yes, one crest-limited step; refuses submergence |
| `rateCalibratedBroadCrestedSpill()` / `stepCalibratedBroadCrestedSpill()` | Same control, with **calibrated tailwater reduction** | Everything above **plus an independently justified submergence rating** | Yes, one bounded explicit step; no separate network/hydrograph |
| `rateFullySubmergedOrifice()` | A rectangular engineered opening whose **entire aperture is underwater on both sides** | Invert, opening width/height, discharge coefficient and component association | **No**: instantaneous discharge rating only |

These are optional core functions, not a change to `AnalysisService`, HTTP endpoints, async jobs, default redistribution policies, stored runoff scenarios, or inundation caches. Their water levels must come from a consistent **vertical datum** and the current state of a separate hydraulic model. There is no automatic inference of pipe locations, opening size, control coefficients, bridge openings, culverts, calibrated rating curves or operating levels from Copernicus GLO-30.

In particular, **a physical orifice may connect two components below their DEM saddle**. That is permitted by this API only because its location and geometry are supplied independently. A DEM contact proves approximate terrain topology, not the existence of an engineered passage.

## 1. Calibrated downstream submergence of a broad-crested weir

The *existing* free-overflow rating is retained unchanged:

\[
Q_{free} = C_d b \sqrt{g}\left(\frac{2H_u}{3}\right)^{3/2}
\]

where `b` is externally provided effective width (m), `C_d` is a **dimensionless** factor in `(0,1]`, `g=9.80665 m/s²`, and `H_u=max(0,z_upstream-z_crest)` (m). Discharge has units of m³/s.

For the **new** opt-in rating, define `H_t=max(0,z_downstream-z_crest)`, the dimensionless ratio `r=H_t/H_u`, and the caller's calibrated, monotonic factor `K(r)`:

\[
Q=\operatorname{sgn}(z_1-z_2)\,Q_{free}\,K(r).
\]

The caller must provide `BroadCrestedSubmergenceRating` knots `(r, K)`:

- First knot `(0,1)` and last knot `(1,0)` are mandatory. Thus at zero positive tailwater head the calculated rate matches the existing free-flow model, and at equal water levels the net flow is zero.
- Ratios must be strictly increasing from 0 to 1. Factors must be finite, nonnegative, nonincreasing and positive before the endpoint.
- The module performs **piecewise-linear interpolation** between knots, not extrapolation, spline overshoot or automatic fitting.
- A control cannot use a calibration measured for a different crest, approach geometry, vertical datum, submergence definition or head convention without independent justification.
- The two-component, cardinal-contact validation of `BroadCrestedSpillControl` still applies. Multiway and diagonal-only merge junctions are not silently converted into rectangular weirs.

### Why an explicit rating curve?

For broad-crested spillways, the appropriate reduction depends on geometry. HEC-RAS uses a broad-crested/roadway **empirical submergence curve**, distinct from the **Villemonte relation associated with sharp-crested weirs**. Accordingly this code does **not** install Villemonte as a universal broad-crested default.

References:

- [USACE HEC-RAS: Submerged Weir Flow](https://www.hec.usace.army.mil/confluence/rasdocs/ras1dtechref/7.0/modeling-gated-spillways-weirs-and-drop-structures/uncontrolled-overflow-weirs/submerged-weir-flow), distinguishes broad-crested and sharp-crested correction families and uses downstream-to-upstream head ratios.
- [USACE HEC-RAS Hydraulic Reference Manual](https://www.hec.usace.army.mil/software/hec-ras/documentation/HEC-RAS%205.0%20Reference%20Manual.pdf), overview of broad-crested coefficient and submergence handling.

The source literature motivates the **type of model**, but **does not validate any user-supplied rating knots**. Values in tests and examples below are deliberately synthetic.

### Fully worked synthetic example

Suppose `z_crest=10 m`, upstream water at `12 m`, downstream water at `11 m`, `b=2 m` and `C_d=0.9`. Then `H_u=2 m`, `H_t=1 m`, `r=0.5` and the ideal adjusted free rate is about **8.678 m³/s**. A hypothetical measured factor of `K(0.5)=0.8` gives a net rate near **6.943 m³/s** from the first component to the second.

These are example inputs—not defaults or recommendations for a real culvert, road, spillway or natural saddle.

## 2. One-step calibrated transfer and mass balance

`stepCalibratedBroadCrestedSpill()` takes two independently supplied, strictly increasing, crest-spanning stage–storage curves, their initial volumes and a positive duration. It:

1. Validates the junction, control, rating, curves, storage bounds, and duration.
2. Inverts stage–storage curves to determine initial water levels and rates.
3. Requests `abs(Q_initial) * duration_seconds` as a **forward-Euler** transfer.
4. Bounds the requested volume by the donor's available storage above the crest, by the receiving curve's remaining supported storage, **and by the volume that would equalize the water levels**. Equalization is found by bisection using the two independent piecewise-linear stage–storage curves.
5. Subtracts the bounded volume from the donor and adds the same volume to the receiver. `transferred_volume_m3` is positive from component one to two, negative in the reverse direction.

Thus, neglecting floating-point rounding, `V1_after+V2_after=V1_before+V2_before`. The existing `limited_by_donor_crest` flag may be set. Two additional flags are available for this new step: `limited_by_recipient_curve` and `limited_by_level_equalization`.

**Crucial limitation:** the rate is held constant at its value **at the start of the step**, although tailwater and headwater evolve as the volumes change. The caps prevent a single step from reversing the hydrostatic head, but they are not an exact integral of the rating curve. A coupled solver will require substeps/adaptive stepping, rainfall and other inflows, simultaneous controls, suitable boundary conditions, and convergence tests. A numerically conserved water volume does **not** imply that the chosen timestep or hydrograph is hydraulically accurate.

### Example in C++

```cpp
#include <aowis/hydrology/spill_hydraulics.h>

using namespace Aowis::Hydrology;

// The junction must be a two-component, cardinally connected saddle.
BroadCrestedSpillControl weir;
weir.first_component_index = junction.child_contacts.at(0).child_basin_index;
weir.second_component_index = junction.child_contacts.at(1).child_basin_index;
weir.crest_elevation_m = 10.0;
weir.effective_width_m = 2.0; // Surveyed, never copied from raster contact length.
weir.discharge_factor = 0.9;  // Calibrated dimensionless multiplier.

// SYNTHETIC example knots, not a real hydraulic calibration.
BroadCrestedSubmergenceRating tailwater;
tailwater.points = {{0.0, 1.0}, {0.5, 0.8}, {0.9, 0.3}, {1.0, 0.0}};

const SpillDischarge rate = rateCalibratedBroadCrestedSpill(
    junction, weir, tailwater, 12.0, 11.0);

const SpillReservoirCurve basin_curve = {
    {{0.0, 0.0}, {10.0, 1000.0}, {20.0, 2000.0}}
};
const SpillTransferStep step = stepCalibratedBroadCrestedSpill(
    junction, weir, tailwater, basin_curve, basin_curve,
    1200.0, 1100.0, 0.1);
// Verify step.calculated and the selected rate regime before using output.
```

## 3. Fully submerged engineered rectangular opening

For a surveyed opening with invert `z_invert`, width `b` and height `h`, the crown is `z_crown=z_invert+h`. When **both** water surfaces are at or above the crown, the module computes:

\[
Q=C_d\,(b h)\,\sqrt{2g\lvert z_1-z_2\rvert},
\]

with discharge direction given by the higher surface. `C_d` must be a defensible **dimensionless** opening coefficient in `(0,1]`. Width/height are in metres and the resulting rate in m³/s. The two `SpillDischarge` diagnostic head fields are relative to the **opening crown** for this orifice (they are relative to the **weir crest** for broad-crested controls).

- Both levels at/above crown and unequal: `FullySubmergedOrifice` and a finite signed rate.
- Identical levels: `NoFlow` with zero rate.
- Both levels at/below invert: dry opening, `NoFlow`.
- Otherwise, at least one surface does not completely submerge the aperture: `PartiallySubmergedUnsupported` and **NaN discharge**. This includes potentially free-discharging openings. No guess is made using the submerged-orifice law.
- The junction must contain exactly the two specified components; unlike the **surface weir**, a culvert may be below the DEM saddle and cannot be required to have the same side-contact geometry.

For example, width `2 m`, height `0.5 m`, `C_d=0.6`, levels `12 m` and `11 m`, a **completely drowned** opening gives about `2.658 m³/s` in the positive direction. This idealized law neglects entrance/exit losses beyond the coefficient, conduit friction, barrel length, air entrainment, valve operation, partial-flow regimes and velocity-head corrections. **It is not a culvert-network solver**; there is no time-stepping method for engineered openings in this increment.

```cpp
RectangularOrificeControl opening;
opening.first_component_index = junction.child_contacts.at(0).child_basin_index;
opening.second_component_index = junction.child_contacts.at(1).child_basin_index;
opening.invert_elevation_m = 4.0; // External engineering data, below DEM saddle.
opening.opening_width_m = 2.0;
opening.opening_height_m = 0.5;
opening.discharge_factor = 0.6;

const SpillDischarge orifice = rateFullySubmergedOrifice(
    junction, opening, 12.0, 11.0);
```

### Reference context

The standard submerged-orifice expression arises from the energy equation plus an empirical discharge coefficient; in realistic engineered culverts other losses can dominate. For broader hydrodynamic treatment consult the [USACE HEC-RAS Hydraulic Reference Manual](https://www.hec.usace.army.mil/software/hec-ras/documentation/HEC-RAS%205.0%20Reference%20Manual.pdf). The present code does **not** claim equivalence to HEC-RAS culvert routing or use its advanced culvert algorithms.

## 4. Error handling, availability and provenance

| Condition | Outcome |
| --- | --- |
| Missing/nonmonotonic tailwater knots, invalid endpoints, or too many knots | `std::invalid_argument` |
| Nonfinite, out-of-range geometry, or mismatched basin indices | `std::invalid_argument` |
| Missing or decreasing stage–storage knots; crest/volume outside curves; nonpositive dt | `std::invalid_argument` |
| Legacy free-flow API presented with unequal stages above crest | `SubmergedUnsupported`, NaN, no transfer — **unchanged** |
| Calibrated weir, submerged with known rating | `SubmergedCalibrated`, finite rate and optional capped transfer |
| Calibrated weir, equal levels or no upstream head | `NoFlow`, zero net discharge |
| Fully submerged orifice, both water levels above crown | `FullySubmergedOrifice`, finite signed rate |
| Orifice with unsupported partial opening | `PartiallySubmergedUnsupported`, NaN — never silently treated as zero |
| Numerical overflow of rated discharge or `Q × dt` | `std::invalid_argument` |

The reference data needed to trust a particular control **does not live in this C++ primitive**. Any future service or persistent project model should carry: connection/component IDs, terrain revision/vertical datum, surveyed physical geometry with provenance, discharge coefficients/rating source, stage–storage validity range, supported flow regimes and calibration date/version. Those attributes must participate in the model and cache revision/fingerprint, not just in transient client configuration.

## 5. Verification and future work

The `aowis-hydrology-submerged-controls` test covers free/partial/submerged regimes; interpolated reduction factors; forward/reverse discharge; unchanged legacy behavior; short and very long transfer steps; donor, receiver and equal-stage limits; volume conservation under randomized workloads; malformed ratings; full-submergence requirements; engineered-opening validation; and unsupported conditions.

Run:

```bash
./tools/zip_hydrology.sh
./compile_linux_tests.sh --both
```

This increases the Qt/GDAL-enabled Hydrology test count from **12 to 13**. The new test is core-only; no Qt, GUI, Map, Standalone or REST component is modified.

Next stages must handle physically based partial-opening/weir regimes where appropriate; multiway engineered junctions; frictional culvert/barrel losses; time-dependent rainfall inflows and outfalls; coupled multi-reservoir stepping with adaptive convergence checks; and independent reference-data/observational validation. Only after those are complete should the persistent scenario solver switch to a dynamic hydraulic policy.
