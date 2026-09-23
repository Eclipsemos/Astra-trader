#include "astra/risk.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

astra::MarketEvent market() {
    return {.sequence = 1,
            .exchange_time_ns = 1'000,
            .receive_time_ns = 2'000,
            .best_bid_units = 100,
            .best_ask_units = 101,
            .bid_quantity_units = 10,
            .ask_quantity_units = 10,
            .trade_price_units = std::nullopt,
            .trade_quantity_units = 0,
            .aggressor_side = std::nullopt};
}

astra::LedgerState ledger() { return {.cash_units = 100'000, .equity_units = 100'000}; }

}  // namespace

TEST_CASE("risk rejects stale and oversized paper orders", "[risk]") {
    const astra::RiskEngine risk({.maximum_order_quantity_units = 10,
                                  .maximum_order_notional_units = 1'000,
                                  .maximum_absolute_position_units = 20,
                                  .maximum_data_age_ns = 500,
                                  .maximum_spread_ppm = 20'000},
                                 {.account_scale = 0,
                                  .price_scale = 0,
                                  .quantity_scale = 0});
    auto request = astra::OrderRequest{.client_order_id = "risk-1",
                                       .side = astra::Side::buy,
                                       .type = astra::OrderType::market,
                                       .quantity_units = 11,
                                       .limit_price_units = std::nullopt,
                                       .created_time_ns = 0,
                                       .reduce_only = false};
    CHECK(risk.evaluate(request, market(), ledger(), 2'000) == astra::RiskDecision::order_limit);
    request.quantity_units = 1;
    CHECK(risk.evaluate(request, market(), ledger(), 2'501) == astra::RiskDecision::stale_market);
}

TEST_CASE("risk accepts a bounded order", "[risk]") {
    const astra::RiskEngine risk({.maximum_order_quantity_units = 10,
                                  .maximum_order_notional_units = 1'000,
                                  .maximum_absolute_position_units = 20,
                                  .maximum_data_age_ns = 500,
                                  .maximum_spread_ppm = 20'000},
                                 {.account_scale = 0,
                                  .price_scale = 0,
                                  .quantity_scale = 0});
    const auto request = astra::OrderRequest{.client_order_id = "risk-2",
                                             .side = astra::Side::buy,
                                             .type = astra::OrderType::market,
                                             .quantity_units = 5,
                                             .limit_price_units = std::nullopt,
                                             .created_time_ns = 0,
                                             .reduce_only = false};
    CHECK(risk.evaluate(request, market(), ledger(), 2'000) == astra::RiskDecision::accept);
}
