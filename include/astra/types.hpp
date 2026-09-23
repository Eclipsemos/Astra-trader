#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace astra {

enum class Side : std::uint8_t { buy, sell };
enum class OrderType : std::uint8_t { market, limit, post_only };
enum class OrderStatus : std::uint8_t {
    pending,
    open,
    partially_filled,
    filled,
    cancel_pending,
    cancelled,
    rejected,
};

[[nodiscard]] constexpr std::int64_t signed_quantity(Side side,
                                                     std::int64_t quantity) noexcept {
    return side == Side::buy ? quantity : -quantity;
}

struct MarketEvent {
    std::uint64_t sequence{0};
    std::int64_t exchange_time_ns{0};
    std::int64_t receive_time_ns{0};
    std::int64_t best_bid_units{0};
    std::int64_t best_ask_units{0};
    std::int64_t bid_quantity_units{0};
    std::int64_t ask_quantity_units{0};
    std::optional<std::int64_t> trade_price_units;
    std::int64_t trade_quantity_units{0};
    std::optional<Side> aggressor_side;
};

struct OrderRequest {
    std::string client_order_id;
    Side side{Side::buy};
    OrderType type{OrderType::market};
    std::int64_t quantity_units{0};
    std::optional<std::int64_t> limit_price_units;
    std::int64_t created_time_ns{0};
    bool reduce_only{false};
};

struct PaperOrder {
    std::uint64_t order_id{0};
    OrderRequest request;
    OrderStatus status{OrderStatus::pending};
    std::int64_t remaining_quantity_units{0};
    std::int64_t filled_quantity_units{0};
    std::int64_t queue_ahead_units{0};
    std::int64_t active_time_ns{0};
    std::optional<std::int64_t> cancel_due_time_ns;
    std::string reject_reason;
};

struct PaperFill {
    std::uint64_t fill_id{0};
    std::uint64_t order_id{0};
    std::string client_order_id;
    std::uint64_t market_sequence{0};
    std::int64_t exchange_time_ns{0};
    Side side{Side::buy};
    std::int64_t quantity_units{0};
    std::int64_t price_units{0};
    std::int64_t fee_units{0};
    std::int64_t realized_pnl_units{0};
    bool maker{false};
};

}  // namespace astra
