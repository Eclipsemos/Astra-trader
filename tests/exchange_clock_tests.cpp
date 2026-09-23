#include "astra/exchange_clock.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("exchange clock compensates a stable fixed host offset") {
    astra::ExchangeClock clock(3, 2'000'000'000);
    CHECK_FALSE(clock.observe(1'000, 76'001'000).stable);
    CHECK_FALSE(clock.observe(2'000, 76'002'100).stable);
    const auto ready = clock.observe(3'000, 76'003'200);
    CHECK(ready.stable);
    CHECK(ready.offset_ns == 76'000'000);
    CHECK(ready.adjusted_age_ns == 200);
}

TEST_CASE("exchange clock rejects a latency spike after warmup") {
    astra::ExchangeClock clock(2, 1'000);
    static_cast<void>(clock.observe(1'000, 11'000));
    CHECK(clock.observe(2'000, 12'100).stable);
    const auto delayed = clock.observe(3'000, 15'000);
    CHECK_FALSE(delayed.stable);
    CHECK(delayed.adjusted_age_ns == 2'000);
}
