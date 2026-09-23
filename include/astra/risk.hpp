#pragma once

#include "astra/ledger.hpp"
#include "astra/types.hpp"

#include <cstdint>
#include <string_view>

namespace astra {

struct RiskLimits {
    std::int64_t maximum_order_quantity_units{0};
    std::int64_t maximum_order_notional_units{0};
    std::int64_t maximum_absolute_position_units{0};
    std::int64_t maximum_data_age_ns{0};
    std::uint32_t maximum_spread_ppm{0};
    std::int64_t maximum_drawdown_units{0};
};

enum class RiskDecision : std::uint8_t {
    accept,
    invalid_market,
    stale_market,
    invalid_quantity,
    order_limit,
    position_limit,
    spread_limit,
    drawdown_limit,
    reduce_only_violation,
};

class RiskEngine final {
public:
    RiskEngine(RiskLimits limits, LedgerConfig ledger_config) noexcept
        : limits_(limits), ledger_config_(ledger_config) {}

    [[nodiscard]] RiskDecision evaluate(const OrderRequest& request,
                                        const MarketEvent& market,
                                        const LedgerState& ledger,
                                        std::int64_t decision_time_ns) const noexcept;

private:
    RiskLimits limits_;
    LedgerConfig ledger_config_;
};

[[nodiscard]] std::string_view to_string(RiskDecision decision) noexcept;

}  // namespace astra
