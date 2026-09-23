#include "astra/feature_builder.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace astra {
namespace {

#if defined(__SIZEOF_INT128__)
__extension__ using Wide = __int128;
#endif

[[nodiscard]] std::int64_t clamp_to_i64(Wide value) noexcept {
    value = std::max(value, static_cast<Wide>(std::numeric_limits<std::int64_t>::min()));
    value = std::min(value, static_cast<Wide>(std::numeric_limits<std::int64_t>::max()));
    return static_cast<std::int64_t>(value);
}

}  // namespace

FeatureBuilder::FeatureBuilder(std::size_t return_lookback_events)
    : midpoints_(return_lookback_events) {
    if (return_lookback_events == 0U) {
        throw std::invalid_argument("return lookback must be positive");
    }
}

ModelState FeatureBuilder::update(const MarketEvent& event,
                                  std::int64_t current_position_units) {
    const auto midpoint = event.best_bid_units +
                          (event.best_ask_units - event.best_bid_units) / 2;
    std::int64_t return_ppm = 0;
    if (size_ == midpoints_.size()) {
        const auto oldest = midpoints_[next_index_];
        if (oldest > 0) {
            return_ppm = clamp_to_i64((static_cast<Wide>(midpoint) - oldest) * 1'000'000 /
                                      oldest);
        }
    }
    midpoints_[next_index_] = midpoint;
    next_index_ = (next_index_ + 1U) % midpoints_.size();
    size_ = std::min(size_ + 1U, midpoints_.size());

    std::int64_t imbalance_ppm = 0;
    const Wide liquidity = static_cast<Wide>(event.bid_quantity_units) +
                           event.ask_quantity_units;
    if (liquidity > 0) {
        imbalance_ppm = clamp_to_i64(
            (static_cast<Wide>(event.bid_quantity_units) - event.ask_quantity_units) *
            1'000'000 / liquidity);
    }

    return {
        .sequence = event.sequence,
        .exchange_time_ns = event.exchange_time_ns,
        .state_age_ns = event.receive_time_ns - event.exchange_time_ns,
        .best_bid_units = event.best_bid_units,
        .best_ask_units = event.best_ask_units,
        .bid_quantity_units = event.bid_quantity_units,
        .ask_quantity_units = event.ask_quantity_units,
        .return_100_events_ppm = return_ppm,
        .flow_imbalance_ppm = imbalance_ppm,
        .current_position_units = current_position_units,
    };
}

}  // namespace astra
