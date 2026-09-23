#include "astra/ledger.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("perpetual ledger accounts long, close, and fee", "[ledger]") {
    astra::PerpetualLedger ledger({
        .initial_cash_units = 100'000,
        .account_scale = 2,
        .price_scale = 2,
        .quantity_scale = 0,
    });

    REQUIRE(ledger.apply_fill(10, 1'000, 100).succeeded());
    REQUIRE(ledger.state().position_units == 10);
    REQUIRE(ledger.state().cash_units == 99'900);
    REQUIRE(ledger.state().total_fee_units == 100);
    REQUIRE(ledger.apply_fill(-10, 1'100, 110).succeeded());
    CHECK(ledger.state().position_units == 0);
    CHECK(ledger.state().realized_pnl_units == 1'000);
    CHECK(ledger.state().cash_units == 100'790);
    CHECK(ledger.state().total_fee_units == 210);
    CHECK(ledger.state().equity_units == ledger.state().cash_units);
}

TEST_CASE("perpetual ledger handles reversal and mark", "[ledger]") {
    astra::PerpetualLedger ledger({.initial_cash_units = 10'000});
    REQUIRE(ledger.apply_fill(10, 100, 0).succeeded());
    REQUIRE(ledger.apply_fill(-20, 90, 0).succeeded());
    CHECK(ledger.state().position_units == -10);
    CHECK(ledger.state().realized_pnl_units == -100);
    REQUIRE(ledger.mark_to_market(80) == astra::LedgerError::ok);
    CHECK(ledger.state().unrealized_pnl_units == 100);
    CHECK(ledger.state().equity_units == 10'000);
}

TEST_CASE("invalid ledger fill leaves state unchanged", "[ledger]") {
    astra::PerpetualLedger ledger({.initial_cash_units = 10'000});
    const auto before = ledger.state();
    CHECK_FALSE(ledger.apply_fill(0, 100, 0).succeeded());
    CHECK(ledger.state().position_units == before.position_units);
    CHECK_FALSE(ledger.mark_to_market(0) == astra::LedgerError::ok);
}
