#include "astra/paper_exchange.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

astra::PaperExchange make_exchange(std::int64_t ack_latency = 0,
                                   std::int64_t cancel_latency = 10) {
    return astra::PaperExchange({
        .ledger = {.initial_cash_units = 1'000'000,
                   .account_scale = 2,
                   .price_scale = 2,
                   .quantity_scale = 0},
        .risk = {.maximum_order_quantity_units = 100,
                 .maximum_order_notional_units = 500'000,
                 .maximum_absolute_position_units = 100,
                 .maximum_data_age_ns = 1'000,
                 .maximum_spread_ppm = 20'000,
                 .maximum_drawdown_units = 100'000},
        .quantity_step_units = 1,
        .minimum_notional_units = 100,
        .maker_fee_ppm = 200,
        .taker_fee_ppm = 500,
        .market_slippage_ppm = 100,
        .acknowledgement_latency_ns = ack_latency,
        .cancellation_latency_ns = cancel_latency,
    });
}

astra::MarketEvent event(std::uint64_t sequence, std::int64_t time,
                         std::int64_t bid = 10'000, std::int64_t ask = 10'001) {
    return {.sequence = sequence,
            .exchange_time_ns = time,
            .receive_time_ns = time + 1,
            .best_bid_units = bid,
            .best_ask_units = ask,
            .bid_quantity_units = 10,
            .ask_quantity_units = 10,
            .trade_price_units = std::nullopt,
            .trade_quantity_units = 0,
            .aggressor_side = std::nullopt};
}

}  // namespace

TEST_CASE("paper exchange fills a post-only order through a later trade", "[paper]") {
    auto exchange = make_exchange();
    REQUIRE(exchange.on_market_event(event(1, 100)));
    const auto submitted = exchange.submit(
        {.client_order_id = "order-1",
         .side = astra::Side::buy,
         .type = astra::OrderType::post_only,
         .quantity_units = 5,
         .limit_price_units = 10'000,
         .created_time_ns = 101},
        101);
    REQUIRE(submitted.status == astra::OrderStatus::open);

    auto trade = event(2, 200, 9'999, 10'000);
    trade.trade_price_units = 10'000;
    trade.trade_quantity_units = 20;
    trade.aggressor_side = astra::Side::sell;
    REQUIRE(exchange.on_market_event(trade));
    REQUIRE(exchange.fills().size() == 1U);
    CHECK(exchange.fills().front().maker);
    CHECK(exchange.orders().front().status == astra::OrderStatus::filled);
    CHECK(exchange.ledger().state().position_units == 5);
}

TEST_CASE("paper exchange honors queue ahead and duplicate sequence", "[paper]") {
    auto exchange = make_exchange();
    REQUIRE(exchange.on_market_event(event(1, 100)));
    REQUIRE(exchange.submit(
                {.client_order_id = "order-queue",
                 .side = astra::Side::buy,
                 .type = astra::OrderType::limit,
                 .quantity_units = 5,
                 .limit_price_units = 10'000,
                 .created_time_ns = 101},
                101)
                .status == astra::OrderStatus::open);

    auto trade = event(2, 200);
    trade.trade_price_units = 10'000;
    trade.trade_quantity_units = 12;
    trade.aggressor_side = astra::Side::sell;
    REQUIRE(exchange.on_market_event(trade));
    REQUIRE(exchange.fills().size() == 1U);
    CHECK(exchange.fills().front().quantity_units == 2);
    CHECK(exchange.orders().front().queue_ahead_units == 0);
    CHECK_FALSE(exchange.on_market_event(trade));
    CHECK(exchange.ignored_market_events() == 1U);
}

TEST_CASE("paper exchange cancellation is delayed and idempotent", "[paper]") {
    auto exchange = make_exchange(0, 50);
    REQUIRE(exchange.on_market_event(event(1, 100)));
    REQUIRE(exchange.submit(
                {.client_order_id = "order-cancel",
                 .side = astra::Side::sell,
                 .type = astra::OrderType::post_only,
                 .quantity_units = 5,
                 .limit_price_units = 10'001,
                 .created_time_ns = 101},
                101)
                .status == astra::OrderStatus::open);
    CHECK(exchange.cancel("order-cancel", 110));
    CHECK_FALSE(exchange.cancel("order-cancel", 111));
    REQUIRE(exchange.on_market_event(event(2, 120)));
    CHECK(exchange.orders().front().status == astra::OrderStatus::cancel_pending);
    REQUIRE(exchange.on_market_event(event(3, 170)));
    CHECK(exchange.orders().front().status == astra::OrderStatus::cancelled);
}

TEST_CASE("paper exchange rejects duplicate client order IDs", "[paper]") {
    auto exchange = make_exchange();
    REQUIRE(exchange.on_market_event(event(1, 100)));
    const astra::OrderRequest request{.client_order_id = "same-id",
                                      .side = astra::Side::buy,
                                      .type = astra::OrderType::market,
                                      .quantity_units = 2,
                                      .limit_price_units = std::nullopt,
                                      .created_time_ns = 101,
                                      .reduce_only = false};
    const auto first = exchange.submit(request, 101);
    const auto second = exchange.submit(request, 101);
    CHECK_FALSE(first.duplicate);
    CHECK(second.duplicate);
    CHECK(second.order_id == first.order_id);
}
