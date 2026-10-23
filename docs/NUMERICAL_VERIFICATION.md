# Numerical verification and reference problems

## Scope

Hydrology separates **numerical verification** (does the implementation solve
its stated equations accurately?) from **physical validation** (do the model
geometry, parameters, inflow histories and equations predict observed events?).
The reference cases in this document establish the former only. They do not
calibrate a spillway, verify a rainfall hydrograph, validate a real catchment,
or establish a universal safe timestep.

`aowis-hydrology-coupled-convergence` runs deterministic, Qt-independent
synthetic reference problems, including an independent closed-form solution
and a separately implemented Runge–Kutta (RK4) integration. It deliberately
does **not** call Hydrology's production discharge-rating functions for its
reference calculations.

`aowis-hydrology-terrain-reference` supplies a mathematically known sloping
DEM and a rectangular enclosed depression. The tests are distinct from
comparison to a third-party GIS: external GIS cross-validation and field
measurements remain separate, future work.

## Free outlet: closed-form benchmark

A constant-area basin has storage `S = 100 h` m³, with water-level datum
`h = 0` at the bottom. A surveyed *hypothetical* free-overflow outlet at
`h = 10 m` has width `2 m`, coefficient `0.9`, and downstream stage `5 m`.
The reservoir begins at `h = 14 m` (`S = 1400 m³`) and receives no inflow.

The rating is `Q = K H^(3/2)`, where

- `H = max(0, h - 10 m)`;
- `K = 0.9 × 2 m × sqrt(9.80665 m/s²) × (2/3)^(3/2)`;
- `A = dS/dh = 100 m²`.

Integration of `A dH/dt = -K H^(3/2)` has the **exact** solution

```
H(t) = [ H(0)^(-1/2) + K*t/(2*A) ]^(-2)
S(t) = 1000 m³ + A*H(t)
V_out(t) = S(0) - S(t)
```

The test compares every one-second snapshot over 20 seconds, including
storage, cumulative outlet volume, and interval-average outlet discharge.
It also checks that the largest one-second discharge interval is the first.

## Coupled reservoirs: independently integrated reference

A second synthetic problem uses two reservoirs (`A = 100 m²` each), initially
storing `1700 m³` and `200 m³`. It has a calibrated free-overflow inter-basin
weir (width `1.5 m`, coefficient `0.8`, crest `10 m`), and an exterior outlet
from the first basin (width `0.7 m`, coefficient `0.75`, same crest). The first
basin receives `3 m³/s` and the second receives `1 m³/s` for 15 seconds.

The exact ODEs for this deliberately *free-overflow-only* regime are

```
dS1/dt = +3 - Q_weir(S1) - Q_out(S1)
dS2/dt = +1 + Q_weir(S1)
dV_weir/dt = Q_weir(S1)
dV_out/dt  = Q_out(S1)
```

The **test's own** RK4 integrator integrates these ODEs at `0.001 s`; the
same solution at `0.0005 s` is compared to within `1e-7 m³` at each output
sample. At each snapshot, it asserts that the donor remains above and the
recipient below the crest: no unsupported regime switch is hidden inside
this smooth-reference comparison.

Each production run uses a fixed maximum explicit-Euler substep and outputs
one-second snapshots. We compare both reservoir storages, cumulative
inter-basin transfer, cumulative exterior discharge, and the interval-average
outlet discharge. The volume ledger at every snapshot is independently
checked against `initial + prescribed inflow`.

## Measured convergence

Reference data: [`benchmarks/coupled_convergence_20261023.json`](benchmarks/coupled_convergence_20261023.json), generated using the Release
Qt-free solver with the test executable's `--report` option. Error columns
represent the **maximum absolute error over the entire reference trajectory**,
not a final-state error.

| Maximum step (s) | Analytic single-outlet storage error (m³) | RK4 coupled-storage error (m³) | RK4 transfer error (m³) | RK4 external outflow error (m³) |
|---:|---:|---:|---:|---:|
| 1 | 4.674 | 9.900 | 6.887 | 3.013 |
| 0.5 | 2.296 | 4.838 | 3.366 | 1.472 |
| 0.25 | 1.138 | 2.392 | 1.664 | 0.728 |
| 0.125 | 0.567 | 1.190 | 0.828 | 0.362 |
| 0.0625 | 0.283 | 0.593 | 0.413 | 0.181 |

The production method exhibits approximately **first-order convergence**:
each step halving approximately halves the error. Both reference cases have
mass-balance residuals below `1.3e-12 m³` in these runs, illustrating why
excellent conservation **alone** is not proof of accurate predicted storage
or discharge. The one-second interval-average discharge error also decreases
consistently under refinement (all individual values are in the JSON file).

The current solver checks physical admissibility of each Euler substep, **not
its truncation error**. For this particular reference, a 1-second limit caused
up to `9.90 m³` coupled-storage error even though no step was rejected. This
is not necessarily acceptable for a real calibration; accuracy requirements
must be established for each application. Do not change the production
integrator or claim arbitrary timestep accuracy from these synthetic cases.

### Additional edge cases

The automated suite checks dry/zero-flow controls, exact input-only volume
budgets, approach to the outlet crest with rejected overly large steps,
wetting across an initially dry crest, no negative outflow, and per-snapshot
mass conservation. Existing routing tests separately cover simultaneous
connections, order invariance, inverted gradients, unsupported orifice
regimes and cancellation.

## Known terrain geometry reference

`aowis-hydrology-terrain-reference` constructs:

1. A UTM raster with six east-draining rows of eight 30-m cells; a constant
   2-m elevation drop per column makes the east cardinal slope steeper than
   diagonal alternatives. Every east-edge outlet must have exactly eight
   upstream cells, including itself, giving nominal contributing area
   `8 × 30² = 7200 m²` (with small WGS84/UTM scale corrections).
2. A 5×5 grid at elevation 10 m, with a 3×3 interior at elevation 0 m.
   The nine interior cells must each have 10 m potential fill depth, and
   their nominal total prism volume is `9 × 30² × 10 = 81000 m³`.

These are **constructed mathematical geometry fixtures**; they are not
validation against QGIS, GRASS, WhiteboxTools or a measured watershed.

## Reproducing and interpreting the results

```bash
./compile_linux_tests.sh --both
ctest --test-dir build-linux-tests-release -L numerical-verification --output-on-failure
build-linux-tests-release/aowis-hydrology-coupled-convergence-test --report
```

The `--report` output is machine-readable JSON with selected step sizes,
interval discharge errors, substep counts and conservation residuals. Run it
again on a different compiler/platform if evaluating changes to numerical
algorithms. These references use small grids/ODE systems and do not benchmark
large DEM performance, memory allocation or operational wall-clock speed.

For real decisions, specify an acceptable accuracy for each output of
interest, rerun with the maximum substep halved, and compare trajectories
rather than only the final mass-balance residual. A true adaptive integration
method with an embedded local error estimator could remove the need for
manual refinement, but the present test results do not by themselves define
an appropriate hydrological error tolerance or mandate changing methods.
