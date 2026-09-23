#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace astra {

enum class ModelAction : std::uint8_t { long_position, short_position, hold };

struct ModelState {
    std::uint64_t sequence{0};
    std::int64_t exchange_time_ns{0};
    std::int64_t state_age_ns{0};
    std::int64_t best_bid_units{0};
    std::int64_t best_ask_units{0};
    std::int64_t bid_quantity_units{0};
    std::int64_t ask_quantity_units{0};
    std::int64_t return_100_events_ppm{0};
    std::int64_t flow_imbalance_ppm{0};
    std::int64_t current_position_units{0};
};

struct ModelDecision {
    ModelAction action{ModelAction::hold};
    std::uint32_t long_probability_ppm{0};
    std::uint32_t short_probability_ppm{0};
    std::uint32_t hold_probability_ppm{1'000'000};
    std::int64_t latency_ns{0};
    std::uint64_t state_sequence{0};
    std::string model;
    std::string error;

    [[nodiscard]] bool succeeded() const noexcept { return error.empty(); }
};

[[nodiscard]] std::string_view to_string(ModelAction action) noexcept;

}  // namespace astra
