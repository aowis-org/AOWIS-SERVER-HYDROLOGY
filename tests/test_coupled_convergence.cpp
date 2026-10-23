#ifdef NDEBUG
#undef NDEBUG
#endif
#include <aowis/hydrology/coupled_routing.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace Aowis::Hydrology;

namespace
{
// Independently specified physical reference: V = A * (h - h0) and
// Q = C * b * sqrt(g) * (2 H / 3)^(3/2). No production rating or routing
// functions are used to evaluate the reference solutions.
constexpr double g = 9.80665;
constexpr double area = 100.0;
constexpr double crest = 10.0;

void close(double value, double reference, double tolerance)
{
    if (!(std::isfinite(value) && std::isfinite(reference) &&
          std::abs(value - reference) <= tolerance))
    {
        std::cerr << "Expected " << reference << " +/- " << tolerance
                  << ", got " << value << '\n';
        std::abort();
    }
}

double coefficient(double width, double factor)
{
    return factor * width * std::sqrt(g) * std::pow(2.0 / 3.0, 1.5);
}

double freeDischarge(double level, double width, double factor)
{
    return coefficient(width, factor) * std::pow(std::max(0.0, level - crest), 1.5);
}

CoupledReservoir reservoir(std::uint64_t id, double initial)
{
    CoupledReservoir result;
    result.component_index = id;
    result.initial_storage_m3 = initial;
    result.curve.points = {{0.0, 0.0}, {20.0, 2000.0}};
    return result;
}

CoupledBoundaryOutlet outlet(std::uint64_t id, double width, double factor)
{
    CoupledBoundaryOutlet result;
    result.reservoir_component_index = id;
    result.crest_elevation_m = crest;
    result.downstream_water_level_m = 5.0;
    result.effective_width_m = width;
    result.discharge_factor = factor;
    return result;
}

CoupledConnection connection()
{
    CoupledConnection c;
    c.junction.saddle_elevation_m = crest;
    BasinSpillContact first;
    first.child_basin_index = 1;
    first.cardinal_contact_faces = 1;
    first.cardinal_boundary_length_m = 5.0;
    BasinSpillContact second = first;
    second.child_basin_index = 2;
    c.junction.child_contacts = {first, second};
    c.weir.first_component_index = 1;
    c.weir.second_component_index = 2;
    c.weir.crest_elevation_m = crest;
    c.weir.effective_width_m = 1.5;
    c.weir.discharge_factor = 0.8;
    c.submergence.points = {{0.0, 1.0}, {1.0, 0.0}};
    return c;
}

CoupledRoutingOptions options(double dt)
{
    CoupledRoutingOptions result;
    result.maximum_substep_seconds = dt;
    result.maximum_substeps = 1000000;
    return result;
}

// Exact solution of A dH/dt = -K H^(3/2), for an initially wet,
// constant-area reservoir draining through a broad-crested free outlet.
double exactHeight(double initial_head, double seconds, double width, double factor)
{
    const double root_inverse = 1.0 / std::sqrt(initial_head) +
                                coefficient(width, factor) * seconds / (2.0 * area);
    return 1.0 / (root_inverse * root_inverse);
}

struct Row
{
    double dt = 0.0;
    double analytic_storage_error_m3 = 0.0;
    double analytic_peak_interval_discharge_error_m3_s = 0.0;
    double rk4_storage_error_m3 = 0.0;
    double rk4_transfer_error_m3 = 0.0;
    double rk4_outflow_error_m3 = 0.0;
    double rk4_peak_interval_discharge_error_m3_s = 0.0;
    double max_balance_error_m3 = 0.0;
    std::uint64_t accepted_substeps = 0;
    std::uint64_t rejected_substeps = 0;
};

// Reference ODE for two independent storage volumes. The second reservoir
// remains below the internal weir crest throughout this smooth test, so
// no interpolation of submerged ratings, discontinuities or assumed regime
// switching enters the independent RK4 reference.
using State = std::array<double, 4>; // S1, S2, internal transferred, exterior outflow

State derivative(const State &s, double inflow_first, double inflow_second)
{
    const double internal = freeDischarge(s[0] / area, 1.5, 0.8);
    const double exterior = freeDischarge(s[0] / area, 0.7, 0.75);
    return {inflow_first - internal - exterior,
            inflow_second + internal, internal, exterior};
}

State addScaled(const State &a, const State &b, double multiplier)
{
    return {a[0] + multiplier*b[0], a[1] + multiplier*b[1],
            a[2] + multiplier*b[2], a[3] + multiplier*b[3]};
}

State rk4Step(State s, double step, double inflow_first, double inflow_second)
{
    const State k1 = derivative(s, inflow_first, inflow_second);
    const State k2 = derivative(addScaled(s, k1, step / 2.0), inflow_first, inflow_second);
    const State k3 = derivative(addScaled(s, k2, step / 2.0), inflow_first, inflow_second);
    const State k4 = derivative(addScaled(s, k3, step), inflow_first, inflow_second);
    for (std::size_t i = 0; i < s.size(); ++i)
        s[i] += step * (k1[i] + 2.0 * k2[i] + 2.0 * k3[i] + k4[i]) / 6.0;
    return s;
}

std::vector<State> referenceTrajectory(int substeps_per_second)
{
    // Output every 1s, 15s total. 1000 independent RK4 substeps/output.
    // Inflows are constant rates, equivalent to 45 and 15 m3/15s.
    std::vector<State> result = {{1700.0, 200.0, 0.0, 0.0}};
    for (int second = 0; second < 15; ++second)
    {
        State s = result.back();
        for (int j = 0; j < substeps_per_second; ++j)
            s = rk4Step(s, 1.0 / substeps_per_second, 3.0, 1.0);
        result.push_back(s);
    }
    return result;
}

std::vector<CoupledInflowInterval> intervals(int count, double first, double second)
{
    return std::vector<CoupledInflowInterval>(static_cast<std::size_t>(count),
                                               {1.0, {first, second}});
}

Row evaluate(double dt, const std::vector<State> &reference)
{
    Row row;
    row.dt = dt;
    const CoupledRoutingResult single = routeCoupledReservoirsWithOutlets(
        {reservoir(1, 1400.0)}, {}, {outlet(1, 2.0, 0.9)},
        std::vector<CoupledInflowInterval>(20, {1.0, {0.0}}), options(dt));
    assert(single.snapshots.size() == 21);
    const double first_head = 4.0;
    for (std::size_t i = 0; i < single.snapshots.size(); ++i)
    {
        const auto &s = single.snapshots[i];
        const double expected_storage = 1000.0 + area * exactHeight(first_head, double(i), 2.0, 0.9);
        row.analytic_storage_error_m3 = std::max(row.analytic_storage_error_m3,
                                                   std::abs(s.storage_m3[0] - expected_storage));
        close(s.storage_m3[0] + s.cumulative_boundary_outflow_m3, 1400.0, 1e-7);
        row.max_balance_error_m3 = std::max(row.max_balance_error_m3,
                                            std::abs(s.mass_balance_error_m3));
        if (i != 0)
        {
            const double exact_average = area *
                (exactHeight(first_head, double(i - 1), 2.0, 0.9) -
                 exactHeight(first_head, double(i), 2.0, 0.9));
            const double observed_average = s.boundary_outflow_m3[0] -
                                            single.snapshots[i - 1].boundary_outflow_m3[0];
            row.analytic_peak_interval_discharge_error_m3_s = std::max(
                row.analytic_peak_interval_discharge_error_m3_s,
                std::abs(observed_average - exact_average));
        }
    }
    // Largest 1-second interval-average occurs at the beginning of
    // this strictly decaying hydrograph; no instantaneous peak is reported.
    const double first_interval = single.snapshots[1].boundary_outflow_m3[0];
    for (std::size_t i = 2; i < single.snapshots.size(); ++i)
        assert(first_interval >= single.snapshots[i].boundary_outflow_m3[0] -
                                 single.snapshots[i-1].boundary_outflow_m3[0] - 1e-10);

    const CoupledRoutingResult network = routeCoupledReservoirsWithOutlets(
        {reservoir(1, 1700.0), reservoir(2, 200.0)}, {connection()},
        {outlet(1, 0.7, 0.75)}, intervals(15, 3.0, 1.0), options(dt));
    assert(network.snapshots.size() == reference.size());
    for (std::size_t i = 0; i < reference.size(); ++i)
    {
        const auto &s = network.snapshots[i];
        row.rk4_storage_error_m3 = std::max(row.rk4_storage_error_m3,
            std::max(std::abs(s.storage_m3[0]-reference[i][0]),
                     std::abs(s.storage_m3[1]-reference[i][1])));
        row.rk4_transfer_error_m3 = std::max(row.rk4_transfer_error_m3,
            std::abs(s.connection_transfer_m3[0]-reference[i][2]));
        row.rk4_outflow_error_m3 = std::max(row.rk4_outflow_error_m3,
            std::abs(s.boundary_outflow_m3[0]-reference[i][3]));
        close(s.total_storage_m3 + s.cumulative_boundary_outflow_m3,
              1900.0 + 4.0 * double(i), 1e-7);
        assert(s.water_level_m[1] < crest); // free-overflow reference stays valid
        row.max_balance_error_m3 = std::max(row.max_balance_error_m3,
                                            std::abs(s.mass_balance_error_m3));
        if (i != 0)
        {
            const double expected_average = reference[i][3] - reference[i-1][3];
            const double observed_average = s.boundary_outflow_m3[0] -
                                            network.snapshots[i-1].boundary_outflow_m3[0];
            row.rk4_peak_interval_discharge_error_m3_s = std::max(
                row.rk4_peak_interval_discharge_error_m3_s,
                std::abs(observed_average - expected_average));
        }
    }
    row.accepted_substeps = single.accepted_substeps + network.accepted_substeps;
    row.rejected_substeps = single.rejected_substeps + network.rejected_substeps;
    return row;
}

void testExactDryAndInflow()
{
    // Arbitrary prescribed forcing has an exact result without outlets or
    // internal controls, including a constant low-stage water surface.
    const auto result = routeCoupledReservoirs(
        {reservoir(1, 0.0), reservoir(2, 75.0)}, {},
        {{2.0, {10.0, 0.0}}, {0.5, {5.0, 15.0}}, {3.5, {0.0, 0.0}}}, options(0.125));
    assert(result.snapshots.size() == 4);
    const std::array<double, 4> first = {0.0, 10.0, 15.0, 15.0};
    const std::array<double, 4> second = {75.0, 75.0, 90.0, 90.0};
    for (std::size_t i = 0; i < first.size(); ++i)
    {
        close(result.snapshots[i].storage_m3[0], first[i], 1e-9);
        close(result.snapshots[i].storage_m3[1], second[i], 1e-9);
        close(result.snapshots[i].mass_balance_error_m3, 0.0, 1e-9);
    }
}

void testNearCrestAndDryTransitions()
{
    // Analytical free outflow approaches the crest asymptotically. Even an
    // initially too-large substep must not withdraw water below the crest.
    const auto above = routeCoupledReservoirsWithOutlets(
        {reservoir(1, 1200.0)}, {}, {outlet(1, 2.0, 0.9)},
        {{150.0, {0.0}}}, options(60.0));
    assert(above.rejected_substeps > 0);
    assert(above.snapshots.back().storage_m3[0] >= 1000.0);
    assert(above.snapshots.back().boundary_outflow_m3[0] >= 0.0);
    close(above.snapshots.back().mass_balance_error_m3, 0.0, 1e-7);
    const auto below = routeCoupledReservoirsWithOutlets(
        {reservoir(1, 950.0)}, {}, {outlet(1, 2.0, 0.9)},
        {{20.0, {0.0}}}, options(10.0));
    close(below.snapshots.back().storage_m3[0], 950.0, 1e-12);
    close(below.snapshots.back().boundary_outflow_m3[0], 0.0, 1e-12);

    // A wetting front must not leak through a dry control before the crest
    // is reached, and subsequent inflow must be divided conservatively
    // between storage and external discharge.
    const auto refill = routeCoupledReservoirsWithOutlets(
        {reservoir(1, 950.0)}, {}, {outlet(1, 1.0, 0.8)},
        {{10.0, {50.0}}, {10.0, {100.0}}}, options(0.25));
    close(refill.snapshots[1].storage_m3[0], 1000.0, 1e-9);
    close(refill.snapshots[1].boundary_outflow_m3[0], 0.0, 1e-9);
    assert(refill.snapshots[2].storage_m3[0] > 1000.0);
    assert(refill.snapshots[2].boundary_outflow_m3[0] > 0.0);
    close(refill.snapshots[2].storage_m3[0] +
          refill.snapshots[2].boundary_outflow_m3[0], 1100.0, 1e-7);
}

void printReport(const std::vector<Row> &rows)
{
    std::cout << std::setprecision(14);
    std::cout << "{\n  \"reference\": \"analytic free overflow; independent RK4 0.001s\",\n"
              << "  \"scenarios\": [\"single_free_outlet_20s\", \"two_reservoir_inflow_and_outlet_15s\"],\n"
              << "  \"rows\": [\n";
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const Row &r = rows[i];
        std::cout << "    {\"dt_s\": " << r.dt
                  << ", \"analytic_storage_max_error_m3\": " << r.analytic_storage_error_m3
                  << ", \"analytic_interval_discharge_max_error_m3_s\": " << r.analytic_peak_interval_discharge_error_m3_s
                  << ", \"rk4_storage_max_error_m3\": " << r.rk4_storage_error_m3
                  << ", \"rk4_transfer_max_error_m3\": " << r.rk4_transfer_error_m3
                  << ", \"rk4_outflow_max_error_m3\": " << r.rk4_outflow_error_m3
                  << ", \"rk4_interval_discharge_max_error_m3_s\": " << r.rk4_peak_interval_discharge_error_m3_s
                  << ", \"max_mass_balance_error_m3\": " << r.max_balance_error_m3
                  << ", \"accepted_substeps\": " << r.accepted_substeps
                  << ", \"rejected_substeps\": " << r.rejected_substeps << "}"
                  << (i + 1 < rows.size() ? "," : "") << '\n';
    }
    std::cout << "  ]\n}\n";
}
} // namespace

int main(int argc, char **argv)
{
    const bool report = argc == 2 && std::string(argv[1]) == "--report";
    if (argc > 1 && !report) return 2;
    testExactDryAndInflow();
    testNearCrestAndDryTransitions();
    const auto reference = referenceTrajectory(1000);
    const auto finer_reference = referenceTrajectory(2000);
    assert(reference.size() == finer_reference.size());
    for (std::size_t i = 0; i < reference.size(); ++i)
    {
        for (std::size_t component = 0; component < 4; ++component)
            close(reference[i][component], finer_reference[i][component], 1e-7);
        // No extrapolation across an unsupported weir-flow regime is used
        // in the independent reference trajectory.
        assert(reference[i][0] > 1000.0);
        assert(reference[i][1] < 1000.0);
    }
    const std::vector<double> steps = {1.0, 0.5, 0.25, 0.125, 0.0625};
    std::vector<Row> rows;
    for (double step : steps) rows.push_back(evaluate(step, reference));
    // First-order explicit Euler: errors must decrease consistently under
    // refinement, with ratios close to two, but no exact-order assertion on
    // tiny roundoff-dominated quantities.
    for (std::size_t i = 1; i < rows.size(); ++i)
    {
        const Row &coarse = rows[i-1];
        const Row &fine = rows[i];
        assert(fine.analytic_storage_error_m3 < 0.58 * coarse.analytic_storage_error_m3);
        assert(fine.rk4_storage_error_m3 < 0.60 * coarse.rk4_storage_error_m3);
        assert(fine.rk4_transfer_error_m3 < 0.60 * coarse.rk4_transfer_error_m3);
        assert(fine.rk4_outflow_error_m3 < 0.60 * coarse.rk4_outflow_error_m3);
        assert(fine.analytic_peak_interval_discharge_error_m3_s <
               0.60 * coarse.analytic_peak_interval_discharge_error_m3_s);
        assert(fine.rk4_peak_interval_discharge_error_m3_s <
               0.60 * coarse.rk4_peak_interval_discharge_error_m3_s);
    }
    assert(rows.back().analytic_storage_error_m3 < 0.4);
    assert(rows.back().rk4_storage_error_m3 < 0.7);
    assert(rows.back().rk4_transfer_error_m3 < 1.0);
    assert(rows.back().rk4_outflow_error_m3 < 1.0);
    for (const Row &r : rows)
        assert(r.max_balance_error_m3 < 1e-7);
    if (report) printReport(rows);
    return 0;
}
