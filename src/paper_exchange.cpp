#include "astra/paper_exchange.hpp"

#include <algorithm>
#include <limits>

namespace astra {
namespace {

#if defined(__SIZEOF_INT128__)
__extension__ using Wide = __int128;
#endif

inline constexpr std::uint32_t kPartsPerMillion = 1'000'000;

[[nodiscard]] constexpr Wide magnitude(std::int64_t value) noexcept {
    return value < 0 ? -static_cast<Wide>(value) : static_cast<Wide>(value);
}

[[nodiscard]] constexpr Wide power_of_ten(std::uint8_t exponent) noexcept {
    Wide value = 1;
    for (std::uint8_t index = 0; index < exponent; ++index) value *= 10;
    return value;
}

[[nodiscard]] constexpr bool fits(Wide value) noexcept {
    return value >= std::numeric_limits<std::int64_t>::min() &&
           value <= std::numeric_limits<std::int64_t>::max();
}

[[nodiscard]] bool calculate_notional(std::int64_t price_units,
                                      std::int64_t quantity_units,
                                      LedgerConfig config,
                                      std::int64_t& result) noexcept {
    Wide value = static_cast<Wide>(price_units) * magnitude(quantity_units);
    const auto source_scale = static_cast<int>(config.price_scale) +
                              static_cast<int>(config.quantity_scale);
    const auto difference = source_scale - static_cast<int>(config.account_scale);
    if (difference > 0) value /= power_of_ten(static_cast<std::uint8_t>(difference));
    if (difference < 0) value *= power_of_ten(static_cast<std::uint8_t>(-difference));
    if (!fits(value)) return false;
    result = static_cast<std::int64_t>(value);
    return true;
}

[[nodiscard]] bool fee_for(std::int64_t notional_units, std::uint32_t fee_ppm,
                           std::int64_t& result) noexcept {
    const Wide product = static_cast<Wide>(notional_units) * fee_ppm;
    const Wide fee = product == 0 ? 0 : ((product - 1) / kPartsPerMillion) + 1;
    if (!fits(fee)) return false;
    result = static_cast<std::int64_t>(fee);
    return true;
}

[[nodiscard]] bool slipped_price(std::int64_t price_units, Side side,
                                 std::uint32_t slippage_ppm,
                                 std::int64_t& result) noexcept {
    const Wide product = static_cast<Wide>(price_units) * slippage_ppm;
    const Wide slip = product == 0 ? 0 : ((product - 1) / kPartsPerMillion) + 1;
    const Wide price = side == Side::buy ? static_cast<Wide>(price_units) + slip
                                         : static_cast<Wide>(price_units) - slip;
    if (price <= 0 || !fits(price)) return false;
    result = static_cast<std::int64_t>(price);
    return true;
}

[[nodiscard]] bool is_terminal(OrderStatus status) noexcept {
    return status == OrderStatus::filled || status == OrderStatus::cancelled ||
           status == OrderStatus::rejected;
}

}  // namespace

PaperExchange::PaperExchange(PaperExchangeConfig config) noexcept
    : config_(config), ledger_(config.ledger), risk_(config.risk, config.ledger),
      config_valid_(valid_config()) {}

bool PaperExchange::valid_config() const noexcept {
    return ledger_.configuration_status() == LedgerError::ok &&
           config_.quantity_step_units > 0 && config_.minimum_notional_units >= 0 &&
           config_.maker_fee_ppm <= kPartsPerMillion &&
           config_.taker_fee_ppm <= kPartsPerMillion &&
           config_.market_slippage_ppm <= kPartsPerMillion &&
           config_.acknowledgement_latency_ns >= 0 &&
           config_.cancellation_latency_ns >= 0;
}

PaperOrder* PaperExchange::find_order(std::string_view client_order_id) noexcept {
    const auto found = order_index_.find(std::string(client_order_id));
    return found == order_index_.end() ? nullptr : &orders_[found->second];
}

SubmitResult PaperExchange::submit(const OrderRequest& request,
                                   std::int64_t decision_time_ns) noexcept {
    if (auto* existing = find_order(request.client_order_id); existing != nullptr) {
        return {existing->order_id, existing->status, true, "duplicate_client_order_id"};
    }
    if (!config_valid_) return {0, OrderStatus::rejected, false, "invalid_configuration"};
    if (!market_.has_value()) return {0, OrderStatus::rejected, false, "market_unavailable"};
    if (request.client_order_id.empty()) {
        return {0, OrderStatus::rejected, false, "empty_client_order_id"};
    }
    if (request.type != OrderType::market &&
        (!request.limit_price_units.has_value() || *request.limit_price_units <= 0)) {
        return {0, OrderStatus::rejected, false, "invalid_limit_price"};
    }

    const auto risk = risk_.evaluate(request, *market_, ledger_.state(), decision_time_ns);
    if (risk != RiskDecision::accept) {
        return {0, OrderStatus::rejected, false, to_string(risk)};
    }

    auto normalized = request;
    normalized.quantity_units =
        (request.quantity_units / config_.quantity_step_units) * config_.quantity_step_units;
    if (normalized.quantity_units <= 0) {
        return {0, OrderStatus::rejected, false, "quantity_below_step"};
    }
    const auto reference_price = normalized.limit_price_units.value_or(
        normalized.side == Side::buy ? market_->best_ask_units : market_->best_bid_units);
    std::int64_t notional{};
    if (!calculate_notional(reference_price, normalized.quantity_units, config_.ledger,
                            notional)) {
        return {0, OrderStatus::rejected, false, "notional_overflow"};
    }
    if (notional < config_.minimum_notional_units) {
        return {0, OrderStatus::rejected, false, "minimum_notional"};
    }

    PaperOrder order{
        .order_id = next_order_id_++,
        .request = std::move(normalized),
        .status = OrderStatus::pending,
        .remaining_quantity_units = 0,
        .filled_quantity_units = 0,
        .queue_ahead_units = 0,
        .active_time_ns = decision_time_ns + config_.acknowledgement_latency_ns,
        .cancel_due_time_ns = std::nullopt,
        .reject_reason = {},
    };
    order.remaining_quantity_units = order.request.quantity_units;
    const auto index = orders_.size();
    order_index_.emplace(order.request.client_order_id, index);
    orders_.push_back(std::move(order));

    auto& stored = orders_.back();
    if (config_.acknowledgement_latency_ns == 0) activate(stored, *market_);
    return {stored.order_id, stored.status, false,
            stored.reject_reason.empty() ? std::string_view{} : stored.reject_reason};
}

void PaperExchange::activate(PaperOrder& order, const MarketEvent& event) noexcept {
    if (is_terminal(order.status) || event.receive_time_ns < order.active_time_ns) return;
    const bool cancellation_pending = order.status == OrderStatus::cancel_pending;

    if (order.request.type == OrderType::market) {
        const auto top = order.request.side == Side::buy ? event.best_ask_units
                                                        : event.best_bid_units;
        std::int64_t price{};
        if (!slipped_price(top, order.request.side, config_.market_slippage_ppm, price)) {
            order.status = OrderStatus::rejected;
            order.reject_reason = "invalid_slipped_price";
            return;
        }
        fill(order, order.remaining_quantity_units, price, false, event);
        return;
    }

    const auto limit = *order.request.limit_price_units;
    const bool crosses = order.request.side == Side::buy ? limit >= event.best_ask_units
                                                         : limit <= event.best_bid_units;
    if (order.request.type == OrderType::post_only && crosses) {
        order.status = OrderStatus::rejected;
        order.reject_reason = "post_only_would_cross";
        return;
    }
    if (crosses) {
        const auto price = order.request.side == Side::buy ? event.best_ask_units
                                                          : event.best_bid_units;
        fill(order, order.remaining_quantity_units, price, false, event);
        return;
    }

    if (order.request.side == Side::buy && limit == event.best_bid_units) {
        order.queue_ahead_units = event.bid_quantity_units;
    } else if (order.request.side == Side::sell && limit == event.best_ask_units) {
        order.queue_ahead_units = event.ask_quantity_units;
    }
    order.status = cancellation_pending ? OrderStatus::cancel_pending : OrderStatus::open;
}

void PaperExchange::fill(PaperOrder& order, std::int64_t quantity_units,
                         std::int64_t price_units, bool maker,
                         const MarketEvent& event) noexcept {
    const auto quantity = std::min(quantity_units, order.remaining_quantity_units);
    if (quantity <= 0) return;
    std::int64_t notional{};
    std::int64_t fee{};
    if (!calculate_notional(price_units, quantity, config_.ledger, notional) ||
        !fee_for(notional, maker ? config_.maker_fee_ppm : config_.taker_fee_ppm, fee)) {
        order.status = OrderStatus::rejected;
        order.reject_reason = "fill_arithmetic_overflow";
        return;
    }
    const auto mutation = ledger_.apply_fill(signed_quantity(order.request.side, quantity),
                                             price_units, fee);
    if (!mutation.succeeded()) {
        order.status = OrderStatus::rejected;
        order.reject_reason = to_string(mutation.error);
        return;
    }

    order.remaining_quantity_units -= quantity;
    order.filled_quantity_units += quantity;
    order.status = order.remaining_quantity_units == 0 ? OrderStatus::filled
                                                       : OrderStatus::partially_filled;
    fills_.push_back({
        .fill_id = next_fill_id_++,
        .order_id = order.order_id,
        .client_order_id = order.request.client_order_id,
        .market_sequence = event.sequence,
        .exchange_time_ns = event.exchange_time_ns,
        .side = order.request.side,
        .quantity_units = quantity,
        .price_units = price_units,
        .fee_units = fee,
        .realized_pnl_units = mutation.realized_pnl_units,
        .maker = maker,
    });
}

void PaperExchange::match_trade(const MarketEvent& event) noexcept {
    if (!event.trade_price_units.has_value() || !event.aggressor_side.has_value() ||
        event.trade_quantity_units <= 0) {
        return;
    }
    auto available = event.trade_quantity_units;
    for (auto& order : orders_) {
        if (available <= 0) break;
        if (order.status != OrderStatus::open &&
            order.status != OrderStatus::partially_filled &&
            order.status != OrderStatus::cancel_pending) {
            continue;
        }
        if (event.receive_time_ns < order.active_time_ns ||
            order.request.type == OrderType::market) {
            continue;
        }
        const bool side_matches =
            (order.request.side == Side::buy && *event.aggressor_side == Side::sell) ||
            (order.request.side == Side::sell && *event.aggressor_side == Side::buy);
        const auto limit = *order.request.limit_price_units;
        const bool price_crosses = order.request.side == Side::buy
                                       ? *event.trade_price_units <= limit
                                       : *event.trade_price_units >= limit;
        if (!side_matches || !price_crosses) continue;

        const auto ahead_consumed = std::min(order.queue_ahead_units, available);
        order.queue_ahead_units -= ahead_consumed;
        available -= ahead_consumed;
        if (available <= 0) break;
        const auto fill_quantity = std::min(order.remaining_quantity_units, available);
        fill(order, fill_quantity, limit, true, event);
        available -= fill_quantity;
    }
}

void PaperExchange::apply_due_cancels(const MarketEvent& event) noexcept {
    for (auto& order : orders_) {
        if (order.status != OrderStatus::cancel_pending ||
            !order.cancel_due_time_ns.has_value() ||
            event.receive_time_ns < *order.cancel_due_time_ns) {
            continue;
        }
        order.status = OrderStatus::cancelled;
        order.remaining_quantity_units = 0;
    }
}

bool PaperExchange::on_market_event(const MarketEvent& event) noexcept {
    if (!config_valid_ || event.sequence == 0 ||
        (market_.has_value() && event.sequence <= market_->sequence) ||
        event.exchange_time_ns <= 0 || event.receive_time_ns < event.exchange_time_ns ||
        event.best_bid_units <= 0 || event.best_ask_units < event.best_bid_units) {
        ++ignored_market_events_;
        return false;
    }
    market_ = event;
    for (auto& order : orders_) {
        if ((order.status == OrderStatus::pending ||
             order.status == OrderStatus::cancel_pending) &&
            event.receive_time_ns >= order.active_time_ns) {
            activate(order, event);
        }
    }
    match_trade(event);
    apply_due_cancels(event);
    const auto midpoint = event.best_bid_units +
                          (event.best_ask_units - event.best_bid_units) / 2;
    static_cast<void>(ledger_.mark_to_market(midpoint));
    return true;
}

bool PaperExchange::cancel(std::string_view client_order_id,
                           std::int64_t request_time_ns) noexcept {
    auto* order = find_order(client_order_id);
    if (order == nullptr || is_terminal(order->status) ||
        order->status == OrderStatus::cancel_pending) {
        return false;
    }
    order->status = OrderStatus::cancel_pending;
    order->cancel_due_time_ns = request_time_ns + config_.cancellation_latency_ns;
    return true;
}

std::string_view to_string(OrderStatus status) noexcept {
    switch (status) {
        case OrderStatus::pending: return "pending";
        case OrderStatus::open: return "open";
        case OrderStatus::partially_filled: return "partially_filled";
        case OrderStatus::filled: return "filled";
        case OrderStatus::cancel_pending: return "cancel_pending";
        case OrderStatus::cancelled: return "cancelled";
        case OrderStatus::rejected: return "rejected";
    }
    return "unknown";
}

}  // namespace astra
