#pragma once

#include <cstdint>
#include <optional>

namespace astra {

struct ExchangeClockState {
    std::int64_t offset_ns{0};
    std::int64_t adjusted_age_ns{0};
    std::uint64_t samples{0};
    bool stable{false};
};

class ExchangeClock final {
public:
    ExchangeClock(std::uint64_t warmup_samples = 100,
                  std::int64_t maximum_residual_ns = 2'000'000'000) noexcept
        : warmup_samples_(warmup_samples), maximum_residual_ns_(maximum_residual_ns) {}

    [[nodiscard]] ExchangeClockState observe(std::int64_t exchange_time_ns,
                                             std::int64_t receive_time_ns) noexcept;
    [[nodiscard]] ExchangeClockState state() const noexcept { return state_; }

private:
    std::uint64_t warmup_samples_;
    std::int64_t maximum_residual_ns_;
    std::optional<std::int64_t> minimum_offset_ns_;
    ExchangeClockState state_;
};

}  // namespace astra
