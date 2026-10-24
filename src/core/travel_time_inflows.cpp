#include <aowis/hydrology/travel_time_inflows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace Aowis::Hydrology
{
namespace
{
constexpr int Dx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int Dy[8] = {0, 1, 1, 1, 0, -1, -1, -1};

void require(bool condition, const char *message)
{
    if (!condition) throw std::invalid_argument(message);
}

void checkCancellation(const std::atomic<bool> *cancel_requested)
{
    if (cancel_requested != nullptr && cancel_requested->load(std::memory_order_relaxed))
        throw std::runtime_error("D8 travel-time forcing canceled");
}

bool approximatelyEqual(long double a, long double b)
{
    return std::abs(a - b) <= 1e-8L * std::max({1.0L, std::abs(a), std::abs(b)});
}

bool boundary(const ElevationGrid &dem, std::uint64_t cell)
{
    const std::uint64_t x = cell % dem.columns;
    const std::uint64_t y = cell / dem.columns;
    return x == 0 || y == 0 || x + 1 == dem.columns || y + 1 == dem.rows;
}

// Deposit one uniform-rate, delayed rectangular pulse into fixed bins without
// iterating across every intervening bin: partial end bins receive direct
// contributions; whole interior bins use a rate-difference prefix sum.
void depositPulse(std::vector<long double> &partial,
                  std::vector<long double> &whole_bin_difference,
                  std::size_t stride, std::size_t destination,
                  std::size_t bin_count, double bin_width,
                  double start, double end, double volume)
{
    if (volume == 0.0) return;
    if (!std::isfinite(start) || !std::isfinite(end) || end <= start)
        throw std::invalid_argument("Travel-time rainfall pulse has invalid arrival times");
    const std::size_t first = static_cast<std::size_t>(std::floor(start / bin_width));
    const std::size_t last = static_cast<std::size_t>(std::ceil(end / bin_width) - 1.0);
    if (first >= bin_count || last >= bin_count || first > last)
        throw std::runtime_error("Travel-time forcing exceeds output horizon");
    if (first == last)
    {
        partial[first * stride + destination] += volume;
        return;
    }
    const long double duration = static_cast<long double>(end) - start;
    const long double quantity = static_cast<long double>(volume);
    const long double first_end = (static_cast<long double>(first) + 1.0L) * bin_width;
    const long double last_start = static_cast<long double>(last) * bin_width;
    partial[first * stride + destination] += quantity * (first_end - start) / duration;
    partial[last * stride + destination] += quantity * (static_cast<long double>(end) - last_start) / duration;
    if (first + 1 < last)
    {
        const long double interior = quantity * bin_width / duration;
        whole_bin_difference[(first + 1) * stride + destination] += interior;
        whole_bin_difference[last * stride + destination] -= interior;
    }
}
} // namespace

TravelTimeRunoffForcing travelTimeInflowsFromRaster(
    const ElevationGrid &original_dem,
    const DepressionHierarchy &hierarchy,
    const std::vector<double> &runoff_volume_m3,
    const std::vector<CoupledReservoir> &reservoirs,
    const std::vector<TemporalRainfallInterval> &time_profile,
    double event_duration_seconds,
    const TravelTimeOptions &options,
    const std::atomic<bool> *cancel_requested)
{
    checkCancellation(cancel_requested);
    require(std::isfinite(options.effective_velocity_m_per_s) &&
                options.effective_velocity_m_per_s > 0.0 &&
                std::isfinite(options.output_interval_seconds) &&
                options.output_interval_seconds > 0.0 &&
                options.maximum_output_intervals >= 1 &&
                options.maximum_output_intervals <= 10000 &&
                options.maximum_raster_cells >= 1 &&
                options.maximum_raster_cells <= 100000000,
            "Invalid D8 travel-time speed, bin width, or work limits");
    const std::uint64_t cell_count = std::uint64_t(original_dem.columns) * original_dem.rows;
    require(cell_count <= options.maximum_raster_cells &&
                cell_count == runoff_volume_m3.size(),
            "Travel-time raster exceeds cell budget or runoff coverage is mismatched");

    // The existing solver validates the original DEM, basin topology, cell
    // runoff and destination totals. Its *static overflow* is not used here.
    const RunoffRedistribution spatial = redistributeRunoff(
        original_dem, hierarchy, runoff_volume_m3, cancel_requested);
    const TemporalRunoffForcing baseline = temporalInflowsFromRedistribution(
        hierarchy, spatial, reservoirs, time_profile,
        event_duration_seconds, cancel_requested);
    require(time_profile.size() <= 10000 &&
                time_profile.size() <= 300000000 / std::max<std::uint64_t>(1, cell_count),
            "Travel-time source-cell/profile work budget exceeded");

    // The plateau ownership rules must match those of redistribution.cpp:
    // any sink on a connected equal-elevation minimum plateau belongs to its
    // hierarchy leaf, not just the particular minimum_cell seed.
    std::vector<std::int64_t> minimum_owner(std::size_t(cell_count), -1);
    std::queue<std::uint64_t> frontier;
    std::unordered_map<std::uint64_t, std::size_t> leaf_column;
    for (std::size_t i = 0; i < reservoirs.size(); ++i)
        leaf_column.emplace(reservoirs[i].component_index, i);
    std::uint64_t visited_plateau_cells = 0;
    for (const DepressionBasin &basin : hierarchy.basins)
    {
        if (!basin.child_indices.empty()) continue;
        require(minimum_owner[std::size_t(basin.minimum_cell)] < 0,
                "Overlapping minimum plateaus in the depression hierarchy");
        minimum_owner[std::size_t(basin.minimum_cell)] = std::int64_t(basin.index);
        frontier.push(basin.minimum_cell);
        while (!frontier.empty())
        {
            if ((++visited_plateau_cells & 4095U) == 0) checkCancellation(cancel_requested);
            const std::uint64_t current = frontier.front();
            frontier.pop();
            const std::int64_t x = std::int64_t(current % original_dem.columns);
            const std::int64_t y = std::int64_t(current / original_dem.columns);
            for (int d = 0; d < 8; ++d)
            {
                const std::int64_t nx = x + Dx[d];
                const std::int64_t ny = y + Dy[d];
                if (nx < 0 || ny < 0 || nx >= original_dem.columns || ny >= original_dem.rows)
                    continue;
                const std::size_t neighbor = std::size_t(ny) * original_dem.columns + std::size_t(nx);
                if (minimum_owner[neighbor] >= 0 ||
                    original_dem.elevations_m[neighbor] != basin.minimum_elevation_m)
                    continue;
                minimum_owner[neighbor] = std::int64_t(basin.index);
                frontier.push(std::uint64_t(neighbor));
            }
        }
    }

    // Only the dry D8 links are needed; avoid analyze()'s priority flood and
    // its additional per-cell rasters (about 44 bytes/cell of temporaries).
    const std::vector<std::int64_t> dry_downstream =
        dryD8Downstream(original_dem, true, cancel_requested);

    // Reversed topological order propagates each terminal and path distance
    // back to every contributing cell without quadratic per-source tracing.
    std::vector<std::uint8_t> incoming(std::size_t(cell_count), 0);
    std::vector<std::uint64_t> order;
    order.reserve(std::size_t(cell_count));
    for (std::uint64_t cell = 0; cell < cell_count; ++cell)
    {
        if ((cell & 4095U) == 0) checkCancellation(cancel_requested);
        const std::int64_t next = dry_downstream[std::size_t(cell)];
        if (next >= 0)
        {
            require(std::uint64_t(next) < cell_count &&
                        ++incoming[std::size_t(next)] <= 8,
                    "Invalid dry D8 flow link");
        }
    }
    std::queue<std::uint64_t> ready;
    for (std::uint64_t cell = 0; cell < cell_count; ++cell)
        if (incoming[std::size_t(cell)] == 0) ready.push(cell);
    while (!ready.empty())
    {
        const std::uint64_t cell = ready.front();
        ready.pop();
        order.push_back(cell);
        const std::int64_t next = dry_downstream[std::size_t(cell)];
        if (next >= 0 && --incoming[std::size_t(next)] == 0)
            ready.push(std::uint64_t(next));
    }
    if (order.size() != cell_count)
        throw std::runtime_error("Dry D8 flow has a cycle");

    // Each row requires just eight ellipsoidal/geodesic neighbor metrics;
    // no geodesic calculation occurs in the individual cell loop.
    std::vector<std::array<double, 8>> edge_lengths(original_dem.rows);
    for (std::uint32_t row = 0; row < original_dem.rows; ++row)
    {
        if ((row & 255U) == 0) checkCancellation(cancel_requested);
        for (int d = 0; d < 8; ++d)
            edge_lengths[row][std::size_t(d)] = rasterRowNeighborDistanceM(
                original_dem.geometry, original_dem.columns, row, Dx[d], Dy[d]);
    }
    // -1 identifies direct boundary runoff; nonnegative values are reservoir
    // columns, not basin IDs (the input reservoir list may be reordered).
    std::vector<std::int64_t> terminal(std::size_t(cell_count), -2);
    std::vector<double> delay(std::size_t(cell_count), 0.0);
    double longest = 0.0;
    std::vector<long double> basin_totals(reservoirs.size(), 0.0L);
    long double boundary_total = 0.0L;
    for (std::size_t position = order.size(); position > 0; --position)
    {
        if ((position & 4095U) == 0) checkCancellation(cancel_requested);
        const std::uint64_t cell = order[position - 1];
        const std::int64_t next = dry_downstream[std::size_t(cell)];
        if (next < 0)
        {
            if (boundary(original_dem, cell)) terminal[std::size_t(cell)] = -1;
            else
            {
                const std::int64_t leaf = minimum_owner[std::size_t(cell)];
                if (leaf < 0 || !leaf_column.contains(std::uint64_t(leaf)))
                    throw std::runtime_error("Dry D8 sink has no matching leaf reservoir");
                terminal[std::size_t(cell)] = std::int64_t(leaf_column.at(std::uint64_t(leaf)));
            }
        }
        else
        {
            const std::int64_t x = std::int64_t(cell % original_dem.columns);
            const std::int64_t y = std::int64_t(cell / original_dem.columns);
            const std::int64_t nx = next % original_dem.columns;
            const std::int64_t ny = next / original_dem.columns;
            const int dx = int(nx - x);
            const int dy = int(ny - y);
            std::size_t direction = 8;
            for (std::size_t d = 0; d < 8; ++d)
                if (Dx[d] == dx && Dy[d] == dy) { direction = d; break; }
            if (direction == 8 || terminal[std::size_t(next)] < -1)
                throw std::runtime_error("Invalid dry D8 neighbor or terminal ordering");
            terminal[std::size_t(cell)] = terminal[std::size_t(next)];
            delay[std::size_t(cell)] = delay[std::size_t(next)] +
                edge_lengths[std::size_t(y)][direction] / options.effective_velocity_m_per_s;
            if (!std::isfinite(delay[std::size_t(cell)]))
                throw std::invalid_argument("D8 travel time is not finite");
        }
        const double volume = runoff_volume_m3[std::size_t(cell)];
        if (volume > 0.0)
        {
            longest = std::max(longest, delay[std::size_t(cell)]);
            const std::int64_t destination = terminal[std::size_t(cell)];
            if (destination < 0) boundary_total += volume;
            else basin_totals[std::size_t(destination)] += volume;
        }
    }
    require(approximatelyEqual(boundary_total, spatial.direct_boundary_outflow_m3),
            "Delayed boundary destinations disagree with static dry D8 routing");
    for (const DepressionBasin &basin : hierarchy.basins)
    {
        if (!basin.child_indices.empty()) continue;
        const std::size_t col = leaf_column.at(basin.index);
        require(approximatelyEqual(basin_totals[col], spatial.basins[std::size_t(basin.index)].direct_inflow_m3),
                "Delayed basin destinations disagree with static dry D8 routing");
    }

    // Derive the tail from the same floating-point operation order used to
    // deposit pulses. Accepted small profile-duration rounding differences
    // must never silently clip the last delayed arrival.
    std::vector<double> storm_starts;
    storm_starts.reserve(time_profile.size());
    double profile_start = 0.0;
    double horizon = event_duration_seconds;
    for (const TemporalRainfallInterval &interval : time_profile)
    {
        storm_starts.push_back(profile_start);
        horizon = std::max(horizon,
            (profile_start + longest) + interval.duration_seconds);
        profile_start += interval.duration_seconds;
    }
    require(std::isfinite(horizon) && horizon > 0.0 &&
                horizon / options.output_interval_seconds <=
                    static_cast<double>(options.maximum_output_intervals),
            "Delayed runoff horizon exceeds output interval budget");
    const std::size_t bin_count = static_cast<std::size_t>(std::ceil(
        horizon / options.output_interval_seconds));
    const std::size_t stride = reservoirs.size() + 1;
    require(bin_count >= 1 && bin_count <= options.maximum_output_intervals &&
                bin_count <= 5000000 / stride,
            "Delayed runoff exceeds output memory budget");
    const std::size_t entries = bin_count * stride;
    std::vector<long double> partial(entries, 0.0L);
    std::vector<long double> whole_bin_difference(entries, 0.0L);

    // Shift each piecewise-constant time-profile pulse as a whole. Full-bin
    // contributions use prefix sums, yielding O(cells * source_intervals +
    // output_bins * leaf_basins), not O(cells * output_bins).
    for (std::size_t cell = 0; cell < std::size_t(cell_count); ++cell)
    {
        if ((cell & 4095U) == 0) checkCancellation(cancel_requested);
        const double volume = runoff_volume_m3[cell];
        if (volume == 0.0) continue;
        const std::size_t dest = terminal[cell] < 0 ? reservoirs.size() :
            std::size_t(terminal[cell]);
        for (std::size_t t = 0; t < time_profile.size(); ++t)
        {
            const TemporalRainfallInterval &interval = time_profile[t];
            if (interval.runoff_fraction == 0.0) continue;
            const double shifted_start = storm_starts[t] + delay[cell];
            const double shifted_end = shifted_start + interval.duration_seconds;
            depositPulse(partial, whole_bin_difference, stride, dest,
                         bin_count, options.output_interval_seconds,
                         shifted_start, shifted_end,
                         volume * interval.runoff_fraction);
        }
    }

    TravelTimeRunoffForcing result;
    result.event_duration_seconds = event_duration_seconds;
    result.maximum_source_travel_seconds = longest;
    result.output_duration_seconds = horizon;
    result.forcing.generated_runoff_m3 = spatial.generated_runoff_m3;
    result.forcing.delivered_to_basins_m3 = baseline.delivered_to_basins_m3;
    result.forcing.direct_boundary_outflow_m3 = baseline.direct_boundary_outflow_m3;
    result.forcing.basin_inflows.resize(bin_count);
    result.forcing.direct_boundary_outflow_m3_by_interval.assign(bin_count, 0.0);
    std::vector<long double> active(stride, 0.0L);
    long double emitted = 0.0L;
    for (std::size_t t = 0; t < bin_count; ++t)
    {
        if ((t & 127U) == 0) checkCancellation(cancel_requested);
        CoupledInflowInterval &interval = result.forcing.basin_inflows[t];
        const double begin = double(t) * options.output_interval_seconds;
        const double finish = std::min(horizon, double(t + 1) * options.output_interval_seconds);
        interval.duration_seconds = finish - begin;
        require(std::isfinite(interval.duration_seconds) && interval.duration_seconds > 0.0,
                "Invalid final delayed-runoff output bin");
        interval.inflow_volume_m3.assign(reservoirs.size(), 0.0);
        for (std::size_t d = 0; d < stride; ++d)
        {
            const std::size_t index = t * stride + d;
            active[d] += whole_bin_difference[index];
            const long double combined = partial[index] + active[d];
            if (combined < -1e-8L || !std::isfinite(combined))
                throw std::runtime_error("Invalid delayed-runoff bin volume");
            const double quantity = static_cast<double>(std::max(0.0L, combined));
            if (d == reservoirs.size())
                result.forcing.direct_boundary_outflow_m3_by_interval[t] = quantity;
            else
                interval.inflow_volume_m3[d] = quantity;
            emitted += quantity;
        }
    }
    result.forcing.mass_balance_error_m3 = static_cast<double>(
        static_cast<long double>(spatial.generated_runoff_m3) - emitted);
    if (!approximatelyEqual(emitted, spatial.generated_runoff_m3))
        throw std::runtime_error("Delayed runoff does not conserve event volume");
    return result;
}

} // namespace Aowis::Hydrology
