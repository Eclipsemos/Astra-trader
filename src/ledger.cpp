#include "astra/ledger.hpp"

#include <algorithm>
#include <limits>

namespace astra {
namespace {

#if defined(__SIZEOF_INT128__)
__extension__ using Wide = __int128;
#else
#error "astra-trader requires signed 128-bit integer support"
#endif

[[nodiscard]] constexpr Wide magnitude(std::int64_t value) noexcept {
    return value < 0 ? -static_cast<Wide>(value) : static_cast<Wide>(value);
}

[[nodiscard]] constexpr int sign(std::int64_t value) noexcept {
    return (value > 0) - (value < 0);
}

[[nodiscard]] constexpr bool fits(Wide value) noexcept {
    return value >= std::numeric_limits<std::int64_t>::min() &&
           value <= std::numeric_limits<std::int64_t>::max();
}

[[nodiscard]] constexpr Wide power_of_ten(std::uint8_t exponent) noexcept {
    Wide value = 1;
    for (std::uint8_t index = 0; index < exponent; ++index) {
        value *= 10;
    }
    return value;
}

[[nodiscard]] bool rescale(Wide value, std::uint16_t source_scale,
                           std::uint8_t target_scale, std::int64_t& result) noexcept {
    if (source_scale < target_scale) {
        const auto exponent = static_cast<std::uint8_t>(target_scale - source_scale);
        if (__builtin_mul_overflow(value, power_of_ten(exponent), &value)) {
            return false;
        }
    } else {
        auto exponent = static_cast<std::uint16_t>(source_scale - target_scale);
        while (exponent != 0U) {
            const auto chunk = static_cast<std::uint8_t>(
                std::min<std::uint16_t>(exponent, 18U));
            value /= power_of_ten(chunk);
            exponent = static_cast<std::uint16_t>(exponent - chunk);
        }
    }
    if (!fits(value)) {
        return false;
    }
    result = static_cast<std::int64_t>(value);
    return true;
}

[[nodiscard]] bool checked_add(std::int64_t left, Wide right,
                               std::int64_t& result) noexcept {
    Wide sum{};
    if (__builtin_add_overflow(static_cast<Wide>(left), right, &sum) || !fits(sum)) {
        return false;
    }
    result = static_cast<std::int64_t>(sum);
    return true;
}

}  // namespace

PerpetualLedger::PerpetualLedger(LedgerConfig config) noexcept : config_(config) {
    if (config.initial_cash_units < 0 || config.account_scale > 18 ||
        config.price_scale > 18 || config.quantity_scale > 18) {
        config_error_ = LedgerError::invalid_configuration;
        return;
    }
    state_.cash_units = config.initial_cash_units;
    state_.equity_units = config.initial_cash_units;
}

bool PerpetualLedger::notional(std::int64_t quantity_units, std::int64_t price_units,
                               std::int64_t& result) const noexcept {
    Wide product{};
    if (__builtin_mul_overflow(magnitude(quantity_units), static_cast<Wide>(price_units),
                              &product)) {
        return false;
    }
    const auto source_scale = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(config_.price_scale) +
        static_cast<std::uint16_t>(config_.quantity_scale));
    return rescale(product, source_scale, config_.account_scale, result);
}

LedgerError PerpetualLedger::refresh_mark(LedgerState& candidate,
                                          std::int64_t price_units) const noexcept {
    if (price_units <= 0) {
        return LedgerError::invalid_fill;
    }
    std::int64_t marked_value{};
    if (!notional(candidate.position_units, price_units, marked_value)) {
        return LedgerError::arithmetic_overflow;
    }
    const auto cost = magnitude(candidate.position_cost_units);
    const Wide unrealized = candidate.position_units > 0
                                ? static_cast<Wide>(marked_value) - cost
                                : candidate.position_units < 0
                                      ? cost - static_cast<Wide>(marked_value)
                                      : 0;
    std::int64_t equity{};
    if (!fits(unrealized) ||
        !checked_add(candidate.cash_units, unrealized, equity)) {
        return LedgerError::arithmetic_overflow;
    }
    candidate.unrealized_pnl_units = static_cast<std::int64_t>(unrealized);
    candidate.equity_units = equity;
    candidate.last_mark_price_units = price_units;
    return LedgerError::ok;
}

LedgerMutation PerpetualLedger::apply_fill(std::int64_t signed_quantity_units,
                                           std::int64_t price_units,
                                           std::int64_t fee_units) noexcept {
    if (config_error_ != LedgerError::ok || signed_quantity_units == 0 ||
        price_units <= 0 || fee_units < 0) {
        return {.error = LedgerError::invalid_fill};
    }

    std::int64_t fill_notional{};
    if (!notional(signed_quantity_units, price_units, fill_notional)) {
        return {.error = LedgerError::arithmetic_overflow};
    }

    auto candidate = state_;
    const auto old_position = candidate.position_units;
    const auto old_cost = candidate.position_cost_units;
    const bool adds = old_position == 0 || sign(old_position) == sign(signed_quantity_units);
    Wide realized = 0;

    if (adds) {
        if (!checked_add(candidate.position_units, signed_quantity_units,
                         candidate.position_units) ||
            !checked_add(candidate.position_cost_units,
                         static_cast<Wide>(sign(signed_quantity_units)) * fill_notional,
                         candidate.position_cost_units)) {
            return {.error = LedgerError::arithmetic_overflow};
        }
    } else {
        const auto close_quantity = std::min(magnitude(old_position),
                                             magnitude(signed_quantity_units));
        const Wide allocated_cost =
            static_cast<Wide>(old_cost) * close_quantity / magnitude(old_position);
        std::int64_t close_notional{};
        if (!notional(static_cast<std::int64_t>(close_quantity), price_units,
                      close_notional)) {
            return {.error = LedgerError::arithmetic_overflow};
        }
        realized = static_cast<Wide>(sign(old_position)) *
                   (static_cast<Wide>(close_notional) - magnitude(static_cast<std::int64_t>(allocated_cost)));

        const Wide new_position = static_cast<Wide>(old_position) + signed_quantity_units;
        if (!fits(new_position)) {
            return {.error = LedgerError::arithmetic_overflow};
        }
        candidate.position_units = static_cast<std::int64_t>(new_position);
        if (candidate.position_units == 0) {
            candidate.position_cost_units = 0;
        } else if (sign(candidate.position_units) == sign(old_position)) {
            const Wide remaining_cost = static_cast<Wide>(old_cost) - allocated_cost;
            if (!fits(remaining_cost)) {
                return {.error = LedgerError::arithmetic_overflow};
            }
            candidate.position_cost_units = static_cast<std::int64_t>(remaining_cost);
        } else {
            const auto opening_quantity = magnitude(candidate.position_units);
            std::int64_t opening_notional{};
            if (!notional(static_cast<std::int64_t>(opening_quantity), price_units,
                          opening_notional)) {
                return {.error = LedgerError::arithmetic_overflow};
            }
            candidate.position_cost_units =
                sign(candidate.position_units) * opening_notional;
        }
    }

    if (!checked_add(candidate.cash_units, realized - fee_units,
                     candidate.cash_units) ||
        !checked_add(candidate.realized_pnl_units, realized,
                     candidate.realized_pnl_units) ||
        !checked_add(candidate.total_fee_units, fee_units,
                     candidate.total_fee_units)) {
        return {.error = LedgerError::arithmetic_overflow};
    }

    if (candidate.position_units == 0) {
        candidate.position_cost_units = 0;
        candidate.average_entry_price_units = 0;
    } else {
        Wide numerator = magnitude(candidate.position_cost_units);
        const auto exponent = static_cast<int>(config_.price_scale) +
                              static_cast<int>(config_.quantity_scale) -
                              static_cast<int>(config_.account_scale);
        if (exponent >= 0) {
            if (__builtin_mul_overflow(numerator,
                                      power_of_ten(static_cast<std::uint8_t>(exponent)),
                                      &numerator)) {
                return {.error = LedgerError::arithmetic_overflow};
            }
        } else {
            numerator /= power_of_ten(static_cast<std::uint8_t>(-exponent));
        }
        const Wide average = numerator / magnitude(candidate.position_units);
        if (!fits(average)) {
            return {.error = LedgerError::arithmetic_overflow};
        }
        candidate.average_entry_price_units = static_cast<std::int64_t>(average);
    }

    const auto mark_error = refresh_mark(candidate, price_units);
    if (mark_error != LedgerError::ok) {
        return {.error = mark_error};
    }
    state_ = candidate;
    return {
        .error = LedgerError::ok,
        .realized_pnl_units = static_cast<std::int64_t>(realized),
        .fee_units = fee_units,
    };
}

LedgerError PerpetualLedger::mark_to_market(std::int64_t price_units) noexcept {
    if (config_error_ != LedgerError::ok) {
        return config_error_;
    }
    auto candidate = state_;
    const auto error = refresh_mark(candidate, price_units);
    if (error == LedgerError::ok) {
        state_ = candidate;
    }
    return error;
}

std::string_view to_string(LedgerError error) noexcept {
    switch (error) {
        case LedgerError::ok: return "ok";
        case LedgerError::invalid_configuration: return "invalid_configuration";
        case LedgerError::invalid_fill: return "invalid_fill";
        case LedgerError::arithmetic_overflow: return "arithmetic_overflow";
    }
    return "unknown";
}

}  // namespace astra
