#include "astra/feature_builder.hpp"
#include "astra/model_worker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <thread>

namespace {

astra::MarketEvent event(std::uint64_t sequence, std::int64_t bid,
                         std::int64_t ask, std::int64_t bid_quantity,
                         std::int64_t ask_quantity) {
    return {.sequence = sequence,
            .exchange_time_ns = 1'000,
            .receive_time_ns = 1'100,
            .best_bid_units = bid,
            .best_ask_units = ask,
            .bid_quantity_units = bid_quantity,
            .ask_quantity_units = ask_quantity,
            .trade_price_units = std::nullopt,
            .trade_quantity_units = 0,
            .aggressor_side = std::nullopt};
}

}  // namespace

TEST_CASE("feature builder produces causal return and top-book imbalance") {
    astra::FeatureBuilder builder(2);
    static_cast<void>(builder.update(event(1, 99, 101, 3, 1), 0));
    static_cast<void>(builder.update(event(2, 100, 102, 3, 1), 0));
    const auto state = builder.update(event(3, 109, 111, 3, 1), -2);

    CHECK(state.return_100_events_ppm == 100'000);
    CHECK(state.flow_imbalance_ppm == 500'000);
    CHECK(state.current_position_units == -2);
    CHECK(state.state_age_ns == 100);
}

TEST_CASE("model worker keeps at most one pending state") {
    std::promise<void> entered;
    std::promise<void> release;
    auto release_future = release.get_future();
    astra::ModelWorker worker([&](const astra::ModelState& state) {
        if (state.sequence == 1) {
            entered.set_value();
            release_future.wait();
        }
        return astra::ModelDecision{.action = astra::ModelAction::hold,
                                    .hold_probability_ppm = 1'000'000,
                                    .state_sequence = state.sequence,
                                    .model = "test",
                                    .error = {}};
    });

    worker.submit({.sequence = 1});
    entered.get_future().wait();
    worker.submit({.sequence = 2});
    worker.submit({.sequence = 3});
    CHECK(worker.coalesced_requests() == 1);
    release.set_value();

    std::optional<astra::ModelDecision> result;
    for (int attempt = 0; attempt < 100 && (!result.has_value() || result->state_sequence != 3);
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (auto next = worker.try_take(); next.has_value()) result = std::move(next);
    }
    REQUIRE(result.has_value());
    CHECK(result->state_sequence == 3);
}
