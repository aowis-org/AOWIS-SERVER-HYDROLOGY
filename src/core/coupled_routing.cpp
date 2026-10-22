#include <aowis/hydrology/coupled_routing.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace Aowis::Hydrology
{
namespace
{
constexpr double BalanceTolerance = 1e-9;

void require(bool valid, const char *message)
{
    if (!valid) throw std::invalid_argument(message);
}

void checkCancellation(const std::atomic<bool> *cancel_requested)
{
    if (cancel_requested != nullptr && cancel_requested->load())
        throw std::runtime_error("Coupled routing canceled");
}

void validateCurve(const SpillReservoirCurve &curve)
{
    require(curve.points.size() >= 2 && curve.points.size() <= 1000000,
            "Coupled reservoir curve must have 2..1000000 points");
    require(std::isfinite(curve.points.front().water_level_m) &&
            curve.points.front().stored_volume_m3 == 0.0,
            "Stage-storage curve must start with finite elevation and zero volume");
    for (std::size_t i = 1; i < curve.points.size(); ++i)
    {
        require(std::isfinite(curve.points[i].water_level_m) &&
                std::isfinite(curve.points[i].stored_volume_m3) &&
                curve.points[i].water_level_m > curve.points[i - 1].water_level_m &&
                curve.points[i].stored_volume_m3 > curve.points[i - 1].stored_volume_m3,
                "Invalid stage-storage curve knot");
    }
}

double levelAt(const SpillReservoirCurve &curve, double volume)
{
    const std::vector<StageStoragePoint>::const_iterator upper =
        std::lower_bound(curve.points.begin(), curve.points.end(), volume,
            [](const StageStoragePoint &point, double stored)
            { return point.stored_volume_m3 < stored; });
    if (upper == curve.points.begin()) return upper->water_level_m;
    if (upper == curve.points.end()) return curve.points.back().water_level_m;
    const StageStoragePoint &lower = *(upper - 1);
    const double fraction = (volume - lower.stored_volume_m3) /
                            (upper->stored_volume_m3 - lower.stored_volume_m3);
    return lower.water_level_m + fraction *
        (upper->water_level_m - lower.water_level_m);
}

struct ConnectionIndices
{
    std::size_t first = 0;
    std::size_t second = 0;
};

CoupledRoutingSnapshot snapshot(double elapsed_seconds,
                                const std::vector<double> &storage,
                                const std::vector<CoupledReservoir> &reservoirs,
                                const std::vector<double> &transfer,
                                long double initial_total,
                                long double cumulative_inflow)
{
    CoupledRoutingSnapshot result;
    result.elapsed_seconds = elapsed_seconds;
    result.storage_m3 = storage;
    result.connection_transfer_m3 = transfer;
    result.cumulative_inflow_m3 = static_cast<double>(cumulative_inflow);
    result.water_level_m.reserve(storage.size());
    long double total = 0.0L;
    for (std::size_t i = 0; i < storage.size(); ++i)
    {
        result.water_level_m.push_back(levelAt(reservoirs[i].curve, storage[i]));
        total += static_cast<long double>(storage[i]);
    }
    result.total_storage_m3 = static_cast<double>(total);
    result.mass_balance_error_m3 = static_cast<double>(total - initial_total - cumulative_inflow);
    const long double scale = std::max({1.0L, initial_total, cumulative_inflow});
    if (!std::isfinite(result.mass_balance_error_m3) ||
        std::abs(static_cast<long double>(result.mass_balance_error_m3)) >
            static_cast<long double>(BalanceTolerance) * scale)
        throw std::runtime_error("Coupled routing water balance is not conserved");
    return result;
}
} // namespace

CoupledRoutingResult routeCoupledReservoirs(
    const std::vector<CoupledReservoir> &reservoirs,
    const std::vector<CoupledConnection> &connections,
    const std::vector<CoupledInflowInterval> &inflow_intervals,
    const CoupledRoutingOptions &options,
    const std::atomic<bool> *cancel_requested)
{
    require(!reservoirs.empty() && reservoirs.size() <= 100000 &&
            connections.size() <= 200000 &&
            inflow_intervals.size() <= options.maximum_output_intervals &&
            inflow_intervals.size() <= 100000,
            "Coupled routing configuration exceeds size limits");
    require(std::isfinite(options.maximum_substep_seconds) &&
            options.maximum_substep_seconds > 0.0 &&
            options.maximum_substeps > 0 && options.maximum_substeps <= 100000000 &&
            options.maximum_output_intervals > 0,
            "Invalid coupled routing work limits");
    checkCancellation(cancel_requested);

    std::unordered_map<std::uint64_t, std::size_t> lookup;
    lookup.reserve(reservoirs.size());
    std::vector<double> storage;
    storage.reserve(reservoirs.size());
    long double initial_total = 0.0L;
    for (std::size_t i = 0; i < reservoirs.size(); ++i)
    {
        const CoupledReservoir &reservoir = reservoirs[i];
        validateCurve(reservoir.curve);
        require(lookup.emplace(reservoir.component_index, i).second,
                "Duplicate coupled reservoir index");
        require(std::isfinite(reservoir.initial_storage_m3) &&
                reservoir.initial_storage_m3 >= 0.0 &&
                reservoir.initial_storage_m3 <= reservoir.curve.points.back().stored_volume_m3,
                "Initial reservoir storage is outside its stage-storage curve");
        storage.push_back(reservoir.initial_storage_m3);
        initial_total += static_cast<long double>(reservoir.initial_storage_m3);
    }

    std::vector<ConnectionIndices> indices;
    indices.reserve(connections.size());
    for (const CoupledConnection &connection : connections)
    {
        const bool is_weir = connection.type == CoupledControlType::CalibratedBroadCrestedWeir;
        const std::uint64_t first_id = is_weir ? connection.weir.first_component_index :
                                         connection.orifice.first_component_index;
        const std::uint64_t second_id = is_weir ? connection.weir.second_component_index :
                                          connection.orifice.second_component_index;
        const std::unordered_map<std::uint64_t, std::size_t>::const_iterator first = lookup.find(first_id);
        const std::unordered_map<std::uint64_t, std::size_t>::const_iterator second = lookup.find(second_id);
        require(first != lookup.end() && second != lookup.end() && first != second,
                "Connection references unknown or identical reservoirs");
        require(connection.type == CoupledControlType::CalibratedBroadCrestedWeir ||
                connection.type == CoupledControlType::FullySubmergedRectangularOrifice,
                "Unknown coupled hydraulic control type");
        const double first_level = levelAt(reservoirs[first->second].curve, storage[first->second]);
        const double second_level = levelAt(reservoirs[second->second].curve, storage[second->second]);
        // Rate functions enforce physical contact geometry and control
        // parameter bounds even for an initially dry connection.
        if (is_weir)
        {
            rateCalibratedBroadCrestedSpill(connection.junction, connection.weir,
                                           connection.submergence, first_level, second_level);
            for (const std::size_t i : {first->second, second->second})
                require(connection.weir.crest_elevation_m >= reservoirs[i].curve.points.front().water_level_m &&
                        connection.weir.crest_elevation_m <= reservoirs[i].curve.points.back().water_level_m,
                        "Weir crest lies outside a reservoir storage curve");
        }
        else
        {
            rateFullySubmergedOrifice(connection.junction, connection.orifice,
                                      first_level, second_level);
            const double crown = connection.orifice.invert_elevation_m + connection.orifice.opening_height_m;
            for (const std::size_t i : {first->second, second->second})
                require(crown <= reservoirs[i].curve.points.back().water_level_m,
                        "Orifice crown exceeds reservoir curve extent");
        }
        indices.push_back({first->second, second->second});
    }

    long double total_duration = 0.0L;
    for (const CoupledInflowInterval &interval : inflow_intervals)
    {
        require(std::isfinite(interval.duration_seconds) && interval.duration_seconds > 0.0 &&
                interval.inflow_volume_m3.size() == reservoirs.size(),
                "Invalid coupled inflow interval or basin count");
        for (const double volume : interval.inflow_volume_m3)
            require(std::isfinite(volume) && volume >= 0.0,
                    "Coupled inflow volumes must be finite and nonnegative");
        total_duration += static_cast<long double>(interval.duration_seconds);
        require(total_duration <= static_cast<long double>(std::numeric_limits<double>::max()),
                "Cumulative coupled routing duration exceeds numeric range");
    }

    CoupledRoutingResult result;
    result.snapshots.reserve(inflow_intervals.size() + 1);
    std::vector<double> transferred(connections.size(), 0.0);
    long double inflow_total = 0.0L;
    double time = 0.0;
    result.snapshots.push_back(snapshot(time, storage, reservoirs, transferred,
                                        initial_total, inflow_total));
    std::vector<double> changes(reservoirs.size(), 0.0);
    std::vector<double> proposed(connections.size(), 0.0);
    std::vector<double> candidate(storage.size(), 0.0);
    std::vector<double> current_levels(storage.size(), 0.0);
    std::vector<double> candidate_levels(storage.size(), 0.0);

    for (const CoupledInflowInterval &interval : inflow_intervals)
    {
        checkCancellation(cancel_requested);
        double remaining = interval.duration_seconds;
        while (remaining > 0.0)
        {
            checkCancellation(cancel_requested);
            require(result.accepted_substeps + result.rejected_substeps < options.maximum_substeps,
                    "Coupled routing substep budget exhausted");
            double step = std::min(remaining, options.maximum_substep_seconds);
            for (std::size_t i = 0; i < storage.size(); ++i)
                current_levels[i] = levelAt(reservoirs[i].curve, storage[i]);
            for (std::uint32_t retry = 0; ; ++retry)
            {
                checkCancellation(cancel_requested);
                require(retry < 64 && step > 0.0 && std::isfinite(step),
                        "Coupled routing cannot resolve a stable substep");
                std::fill(changes.begin(), changes.end(), 0.0);
                for (std::size_t i = 0; i < reservoirs.size(); ++i)
                    changes[i] = interval.inflow_volume_m3[i] * (step / interval.duration_seconds);
                for (std::size_t c = 0; c < connections.size(); ++c)
                {
                    const CoupledConnection &connection = connections[c];
                    const ConnectionIndices &ends = indices[c];
                    SpillDischarge rate;
                    if (connection.type == CoupledControlType::CalibratedBroadCrestedWeir)
                        rate = rateCalibratedBroadCrestedSpill(
                            connection.junction, connection.weir, connection.submergence,
                            current_levels[ends.first], current_levels[ends.second]);
                    else
                        rate = rateFullySubmergedOrifice(
                            connection.junction, connection.orifice,
                            current_levels[ends.first], current_levels[ends.second]);
                    if (rate.regime == SpillFlowRegime::PartiallySubmergedUnsupported ||
                        rate.regime == SpillFlowRegime::SubmergedUnsupported)
                        throw std::invalid_argument("Unsupported coupled hydraulic flow regime");
                    proposed[c] = rate.signed_discharge_m3_s * step;
                    require(std::isfinite(proposed[c]), "Nonfinite coupled transfer volume");
                    changes[ends.first] -= proposed[c];
                    changes[ends.second] += proposed[c];
                }
                bool acceptable = true;
                for (std::size_t i = 0; i < reservoirs.size(); ++i)
                {
                    candidate[i] = storage[i] + changes[i];
                    if (!std::isfinite(candidate[i]) || candidate[i] < 0.0 ||
                        candidate[i] > reservoirs[i].curve.points.back().stored_volume_m3)
                    {
                        acceptable = false;
                        break;
                    }
                    candidate_levels[i] = levelAt(reservoirs[i].curve, candidate[i]);
                }
                if (acceptable)
                {
                    for (std::size_t c = 0; c < connections.size(); ++c)
                    {
                        if (proposed[c] == 0.0) continue;
                        const ConnectionIndices &ends = indices[c];
                        const double old_difference = current_levels[ends.first] - current_levels[ends.second];
                        const double new_difference = candidate_levels[ends.first] - candidate_levels[ends.second];
                        const double tolerance = 1e-12 * std::max({1.0, std::abs(current_levels[ends.first]),
                                                        std::abs(current_levels[ends.second])});
                        if ((old_difference > 0.0 && new_difference < -tolerance) ||
                            (old_difference < 0.0 && new_difference > tolerance))
                            acceptable = false;
                        if (connections[c].type == CoupledControlType::CalibratedBroadCrestedWeir)
                        {
                            const double donor_level = proposed[c] > 0.0 ?
                                candidate_levels[ends.first] : candidate_levels[ends.second];
                            if (donor_level + tolerance < connections[c].weir.crest_elevation_m)
                                acceptable = false;
                        }
                        else
                        {
                            const double lower_level = std::min(candidate_levels[ends.first],
                                                                candidate_levels[ends.second]);
                            const double crown = connections[c].orifice.invert_elevation_m +
                                                 connections[c].orifice.opening_height_m;
                            if (lower_level + tolerance < crown)
                                acceptable = false;
                        }
                    }
                }
                if (acceptable)
                {
                    // Update all reservoirs simultaneously; no earlier edge in
                    // the vector receives a head-start over later edges.
                    storage.swap(candidate);
                    for (std::size_t c = 0; c < transferred.size(); ++c)
                        transferred[c] += proposed[c];
                    for (std::size_t i = 0; i < reservoirs.size(); ++i)
                        inflow_total += static_cast<long double>(interval.inflow_volume_m3[i]) *
                            static_cast<long double>(step / interval.duration_seconds);
                    const double new_remaining = remaining - step;
                    require(new_remaining < remaining, "Coupled timestep made no progress");
                    remaining = new_remaining;
                    ++result.accepted_substeps;
                    break;
                }
                require(result.accepted_substeps + result.rejected_substeps < options.maximum_substeps,
                        "Coupled routing substep budget exhausted");
                ++result.rejected_substeps;
                step *= 0.5;
            }
        }
        time += interval.duration_seconds;
        result.snapshots.push_back(snapshot(time, storage, reservoirs, transferred,
                                            initial_total, inflow_total));
    }
    return result;
}

} // namespace Aowis::Hydrology
