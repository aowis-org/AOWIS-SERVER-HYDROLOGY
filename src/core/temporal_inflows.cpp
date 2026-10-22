#include <aowis/hydrology/temporal_inflows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace Aowis::Hydrology
{
namespace
{
void require(bool condition, const char *message)
{
    if (!condition) throw std::invalid_argument(message);
}

void checkCancellation(const std::atomic<bool> *cancel_requested)
{
    if (cancel_requested != nullptr && cancel_requested->load(std::memory_order_relaxed))
        throw std::runtime_error("Temporal runoff forcing canceled");
}

bool balanced(long double difference, long double magnitude)
{
    return std::abs(difference) <= 1e-9L * std::max(1.0L, magnitude);
}

// Split one immutable event volume across steps. Assigning the remainder on
// the final step preserves the input volume rather than accumulating rounded
// portions. This also treats all-zero and dry-outlet volumes consistently.
std::vector<double> distribute(double volume, const std::vector<double> &fractions)
{
    std::vector<double> portions(fractions.size(), 0.0);
    long double remaining = static_cast<long double>(volume);
    for (std::size_t i = 0; i < fractions.size(); ++i)
    {
        const long double piece = (i + 1 == fractions.size()) ? remaining :
            static_cast<long double>(volume) * static_cast<long double>(fractions[i]);
        if (piece < 0.0L && !balanced(piece, volume))
            throw std::runtime_error("Temporal inflow split exceeded source volume");
        portions[i] = static_cast<double>(std::max(0.0L, piece));
        remaining -= static_cast<long double>(portions[i]);
    }
    return portions;
}
} // namespace

TemporalRunoffForcing temporalInflowsFromRedistribution(
    const DepressionHierarchy &hierarchy,
    const RunoffRedistribution &spatial_runoff,
    const std::vector<CoupledReservoir> &reservoirs,
    const std::vector<TemporalRainfallInterval> &time_profile,
    double event_duration_seconds,
    const std::atomic<bool> *cancel_requested)
{
    checkCancellation(cancel_requested);
    require(!time_profile.empty() && time_profile.size() <= 10000 &&
            std::isfinite(event_duration_seconds) && event_duration_seconds > 0.0,
            "Temporal forcing requires a positive finite event duration and 1..10000 intervals");
    require(hierarchy.basins.size() == spatial_runoff.basins.size() &&
            hierarchy.basins.size() <= 100000 &&
            reservoirs.size() <= 100000 &&
            (reservoirs.empty() || time_profile.size() <= 5000000 / reservoirs.size()),
            "Temporal forcing hierarchy, spatial result, or reservoir limit mismatch");
    require(std::isfinite(spatial_runoff.generated_runoff_m3) &&
            spatial_runoff.generated_runoff_m3 >= 0.0 &&
            std::isfinite(spatial_runoff.direct_boundary_outflow_m3) &&
            spatial_runoff.direct_boundary_outflow_m3 >= 0.0,
            "Invalid spatial runoff totals");

    std::vector<double> fractions;
    fractions.reserve(time_profile.size());
    long double duration = 0.0L;
    long double fraction_total = 0.0L;
    for (const TemporalRainfallInterval &interval : time_profile)
    {
        require(std::isfinite(interval.duration_seconds) && interval.duration_seconds > 0.0 &&
                std::isfinite(interval.runoff_fraction) && interval.runoff_fraction >= 0.0 &&
                interval.runoff_fraction <= 1.0,
                "Temporal rainfall interval has invalid duration or fraction");
        duration += static_cast<long double>(interval.duration_seconds);
        fraction_total += static_cast<long double>(interval.runoff_fraction);
        fractions.push_back(interval.runoff_fraction);
    }
    require(balanced(duration - event_duration_seconds, event_duration_seconds) &&
            std::abs(fraction_total - 1.0L) <= 1e-12L,
            "Temporal intervals must span event duration and sum to 100% runoff");
    // Small finite summation drift does not change the total event volume.
    for (double &fraction : fractions)
        fraction = static_cast<double>(static_cast<long double>(fraction) / fraction_total);

    std::unordered_map<std::uint64_t, std::size_t> reservoir_columns;
    reservoir_columns.reserve(reservoirs.size());
    for (std::size_t i = 0; i < reservoirs.size(); ++i)
    {
        const std::uint64_t id = reservoirs[i].component_index;
        require(id < hierarchy.basins.size() && hierarchy.basins[std::size_t(id)].child_indices.empty() &&
                reservoir_columns.emplace(id, i).second,
                "Coupled reservoirs must be unique depression hierarchy leaves");
    }

    std::size_t leaf_count = 0;
    long double assigned = 0.0L;
    for (std::size_t i = 0; i < hierarchy.basins.size(); ++i)
    {
        if ((i & 4095U) == 0) checkCancellation(cancel_requested);
        const DepressionBasin &basin = hierarchy.basins[i];
        const BasinRedistributionState &spatial = spatial_runoff.basins[i];
        require(basin.index == i && spatial.basin_index == i &&
                std::isfinite(spatial.direct_inflow_m3) && spatial.direct_inflow_m3 >= 0.0,
                "Invalid basin indices or direct inflow volume");
        if (basin.child_indices.empty())
        {
            ++leaf_count;
            require(reservoir_columns.contains(std::uint64_t(i)),
                    "Every leaf basin must have exactly one coupled reservoir");
            assigned += spatial.direct_inflow_m3;
        }
        else
            require(spatial.direct_inflow_m3 == 0.0,
                    "Parent basin direct inflow would double-count source runoff");
    }
    require(leaf_count == reservoirs.size(),
            "Coupled reservoir count must equal depression leaf count");
    const long double source_total = static_cast<long double>(spatial_runoff.generated_runoff_m3);
    const long double domain_direct = static_cast<long double>(spatial_runoff.direct_boundary_outflow_m3);
    require(balanced(source_total - assigned - domain_direct, source_total),
            "Spatial runoff is not conserved between leaf inflow and direct boundary drainage");

    TemporalRunoffForcing result;
    result.generated_runoff_m3 = spatial_runoff.generated_runoff_m3;
    result.delivered_to_basins_m3 = static_cast<double>(assigned);
    result.direct_boundary_outflow_m3 = spatial_runoff.direct_boundary_outflow_m3;
    result.basin_inflows.resize(time_profile.size());
    for (std::size_t t = 0; t < time_profile.size(); ++t)
    {
        result.basin_inflows[t].duration_seconds = time_profile[t].duration_seconds;
        result.basin_inflows[t].inflow_volume_m3.assign(reservoirs.size(), 0.0);
    }
    for (std::size_t i = 0; i < hierarchy.basins.size(); ++i)
    {
        if (!hierarchy.basins[i].child_indices.empty()) continue;
        if ((i & 4095U) == 0) checkCancellation(cancel_requested);
        const std::size_t column = reservoir_columns.at(std::uint64_t(i));
        const std::vector<double> portions = distribute(
            spatial_runoff.basins[i].direct_inflow_m3, fractions);
        for (std::size_t t = 0; t < portions.size(); ++t)
            result.basin_inflows[t].inflow_volume_m3[column] = portions[t];
    }
    result.direct_boundary_outflow_m3_by_interval = distribute(
        spatial_runoff.direct_boundary_outflow_m3, fractions);
    long double emitted = 0.0L;
    for (std::size_t t = 0; t < time_profile.size(); ++t)
    {
        checkCancellation(cancel_requested);
        emitted += result.direct_boundary_outflow_m3_by_interval[t];
        for (const double volume : result.basin_inflows[t].inflow_volume_m3)
            emitted += volume;
    }
    result.mass_balance_error_m3 = static_cast<double>(source_total - emitted);
    if (!balanced(source_total - emitted, source_total))
        throw std::runtime_error("Temporal runoff forcing violates event mass balance");
    return result;
}

TemporalRunoffForcing temporalInflowsFromRaster(
    const ElevationGrid &original_dem,
    const DepressionHierarchy &hierarchy,
    const std::vector<double> &runoff_volume_m3,
    const std::vector<CoupledReservoir> &reservoirs,
    const std::vector<TemporalRainfallInterval> &time_profile,
    double event_duration_seconds,
    const std::atomic<bool> *cancel_requested)
{
    checkCancellation(cancel_requested);
    const RunoffRedistribution spatial = redistributeRunoff(
        original_dem, hierarchy, runoff_volume_m3, cancel_requested);
    return temporalInflowsFromRedistribution(hierarchy, spatial, reservoirs,
                                             time_profile, event_duration_seconds,
                                             cancel_requested);
}

} // namespace Aowis::Hydrology
