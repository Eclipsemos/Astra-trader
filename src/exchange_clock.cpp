#include "astra/exchange_clock.hpp"

#include <algorithm>

namespace astra {

ExchangeClockState ExchangeClock::observe(std::int64_t exchange_time_ns,
                                          std::int64_t receive_time_ns) noexcept {
    if (exchange_time_ns <= 0 || receive_time_ns < exchange_time_ns ||
        maximum_residual_ns_ <= 0 || warmup_samples_ == 0) {
        state_.stable = false;
        return state_;
    }

    const auto observed_offset = receive_time_ns - exchange_time_ns;
    if (!minimum_offset_ns_.has_value()) {
        minimum_offset_ns_ = observed_offset;
        state_.samples = 1;
    } else if (observed_offset < *minimum_offset_ns_) {
        const auto shift = *minimum_offset_ns_ - observed_offset;
        minimum_offset_ns_ = observed_offset;
        state_.samples = shift > maximum_residual_ns_ ? 1 : state_.samples + 1;
    } else {
        ++state_.samples;
    }

    state_.offset_ns = *minimum_offset_ns_;
    state_.adjusted_age_ns = std::max<std::int64_t>(0, observed_offset - *minimum_offset_ns_);
    state_.stable = state_.samples >= warmup_samples_ &&
                    state_.adjusted_age_ns <= maximum_residual_ns_;
    return state_;
}

}  // namespace astra
