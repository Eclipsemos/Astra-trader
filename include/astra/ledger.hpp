#pragma once

#include <cstdint>
#include <string_view>

namespace astra {

enum class LedgerError : std::uint8_t {
    ok,
    invalid_configuration,
    invalid_fill,
    arithmetic_overflow,
};

struct LedgerConfig {
    std::int64_t initial_cash_units{0};
    std::uint8_t account_scale{0};
    std::uint8_t price_scale{0};
    std::uint8_t quantity_scale{0};
};

struct LedgerState {
    std::int64_t cash_units{0};
    std::int64_t position_units{0};
    std::int64_t position_cost_units{0};
    std::int64_t average_entry_price_units{0};
    std::int64_t realized_pnl_units{0};
    std::int64_t unrealized_pnl_units{0};
    std::int64_t total_fee_units{0};
    std::int64_t equity_units{0};
    std::int64_t last_mark_price_units{0};
};

struct LedgerMutation {
    LedgerError error{LedgerError::ok};
    std::int64_t realized_pnl_units{0};
    std::int64_t fee_units{0};
    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return error == LedgerError::ok;
    }
};

class PerpetualLedger final {
public:
    explicit PerpetualLedger(LedgerConfig config) noexcept;

    [[nodiscard]] LedgerMutation apply_fill(std::int64_t signed_quantity_units,
                                            std::int64_t price_units,
                                            std::int64_t fee_units) noexcept;
    [[nodiscard]] LedgerError mark_to_market(std::int64_t price_units) noexcept;

    [[nodiscard]] LedgerError configuration_status() const noexcept { return config_error_; }
    [[nodiscard]] const LedgerConfig& config() const noexcept { return config_; }
    [[nodiscard]] const LedgerState& state() const noexcept { return state_; }

private:
    [[nodiscard]] bool notional(std::int64_t quantity_units, std::int64_t price_units,
                                std::int64_t& result) const noexcept;
    [[nodiscard]] LedgerError refresh_mark(LedgerState& candidate,
                                           std::int64_t price_units) const noexcept;

    LedgerConfig config_;
    LedgerState state_;
    LedgerError config_error_{LedgerError::ok};
};

[[nodiscard]] std::string_view to_string(LedgerError error) noexcept;

}  // namespace astra
