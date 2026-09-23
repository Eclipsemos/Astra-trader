#include "astra/risk.hpp"

#include <algorithm>
#include <limits>

namespace astra {
namespace {

#if defined(__SIZEOF_INT128__)
__extension__ using Wide = __int128;
#endif

[[nodiscard]] constexpr Wide magnitude(Wide value) noexcept {
    return value < 0 ? -value : value;
}

[[nodiscard]] constexpr Wide power_of_ten(std::uint8_t exponent) noexcept {
    Wide value = 1;
    for (std::uint8_t index = 0; index < exponent; ++index) value *= 10;
    return value;
}

[[nodiscard]] Wide account_notional(const OrderRequest& request,
                                    const MarketEvent& market,
                                    LedgerConfig config) noexcept {
    const auto price = request.limit_price_units.value_or(
        request.side == Side::buy ? market.best_ask_units : market.best_bid_units);
    Wide value = magnitude(request.quantity_units) * price;
    const auto source_scale = static_cast<int>(config.price_scale) +
                              static_cast<int>(config.quantity_scale);
    const auto difference = source_scale - static_cast<int>(config.account_scale);
    if (difference > 0) value /= power_of_ten(static_cast<std::uint8_t>(difference));
    if (difference < 0) value *= power_of_ten(static_cast<std::uint8_t>(-difference));
    return value;
}

}  // namespace

RiskDecision RiskEngine::evaluate(const OrderRequest& request,
                                  const MarketEvent& market,
                                  const LedgerState& ledger,
                                  std::int64_t decision_time_ns) const noexcept {
    if (market.best_bid_units <= 0 || market.best_ask_units < market.best_bid_units) {
        return RiskDecision::invalid_market;
    }
    if (limits_.maximum_data_age_ns > 0 &&
        (decision_time_ns < market.receive_time_ns ||
         decision_time_ns - market.receive_time_ns > limits_.maximum_data_age_ns)) {
        return RiskDecision::stale_market;
    }
    if (request.quantity_units <= 0) {
        return RiskDecision::invalid_quantity;
    }
    if (limits_.maximum_order_quantity_units > 0 &&
        request.quantity_units > limits_.maximum_order_quantity_units) {
        return RiskDecision::order_limit;
    }
    if (limits_.maximum_order_notional_units > 0 &&
        account_notional(request, market, ledger_config_) >
            limits_.maximum_order_notional_units) {
        return RiskDecision::order_limit;
    }

    const Wide delta = request.side == Side::buy ? request.quantity_units
                                                 : -static_cast<Wide>(request.quantity_units);
    const Wide projected = static_cast<Wide>(ledger.position_units) + delta;
    if (limits_.maximum_absolute_position_units > 0 &&
        magnitude(projected) >
            limits_.maximum_absolute_position_units) {
        return RiskDecision::position_limit;
    }
    if (request.reduce_only &&
        (ledger.position_units == 0 || magnitude(projected) >= magnitude(ledger.position_units) ||
         (projected != 0 && ((projected > 0) != (ledger.position_units > 0))))) {
        return RiskDecision::reduce_only_violation;
    }

    const Wide midpoint = (static_cast<Wide>(market.best_bid_units) +
                           market.best_ask_units) / 2;
    const Wide spread_ppm =
        static_cast<Wide>(market.best_ask_units - market.best_bid_units) * 1'000'000 /
        midpoint;
    if (limits_.maximum_spread_ppm > 0 &&
        spread_ppm > limits_.maximum_spread_ppm) {
        return RiskDecision::spread_limit;
    }

    const Wide net_pnl = static_cast<Wide>(ledger.realized_pnl_units) +
                         ledger.unrealized_pnl_units - ledger.total_fee_units;
    if (limits_.maximum_drawdown_units > 0 && net_pnl < -limits_.maximum_drawdown_units) {
        return RiskDecision::drawdown_limit;
    }
    return RiskDecision::accept;
}

std::string_view to_string(RiskDecision decision) noexcept {
    switch (decision) {
        case RiskDecision::accept: return "accept";
        case RiskDecision::invalid_market: return "invalid_market";
        case RiskDecision::stale_market: return "stale_market";
        case RiskDecision::invalid_quantity: return "invalid_quantity";
        case RiskDecision::order_limit: return "order_limit";
        case RiskDecision::position_limit: return "position_limit";
        case RiskDecision::spread_limit: return "spread_limit";
        case RiskDecision::drawdown_limit: return "drawdown_limit";
        case RiskDecision::reduce_only_violation: return "reduce_only_violation";
    }
    return "unknown";
}

}  // namespace astra
