#pragma once

#include "astra/ledger.hpp"
#include "astra/risk.hpp"
#include "astra/types.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace astra {

struct PaperExchangeConfig {
    LedgerConfig ledger;
    RiskLimits risk;
    std::int64_t quantity_step_units{1};
    std::int64_t minimum_notional_units{0};
    std::uint32_t maker_fee_ppm{0};
    std::uint32_t taker_fee_ppm{0};
    std::uint32_t market_slippage_ppm{0};
    std::int64_t acknowledgement_latency_ns{0};
    std::int64_t cancellation_latency_ns{0};
};

struct SubmitResult {
    std::uint64_t order_id{0};
    OrderStatus status{OrderStatus::rejected};
    bool duplicate{false};
    std::string_view reason;
};

class PaperExchange final {
public:
    explicit PaperExchange(PaperExchangeConfig config) noexcept;

    [[nodiscard]] bool on_market_event(const MarketEvent& event) noexcept;
    [[nodiscard]] SubmitResult submit(const OrderRequest& request,
                                      std::int64_t decision_time_ns) noexcept;
    [[nodiscard]] bool cancel(std::string_view client_order_id,
                              std::int64_t request_time_ns) noexcept;

    [[nodiscard]] const PerpetualLedger& ledger() const noexcept { return ledger_; }
    [[nodiscard]] const std::vector<PaperOrder>& orders() const noexcept { return orders_; }
    [[nodiscard]] const std::vector<PaperFill>& fills() const noexcept { return fills_; }
    [[nodiscard]] std::optional<MarketEvent> market() const noexcept { return market_; }
    [[nodiscard]] std::uint64_t ignored_market_events() const noexcept {
        return ignored_market_events_;
    }

private:
    [[nodiscard]] PaperOrder* find_order(std::string_view client_order_id) noexcept;
    void activate(PaperOrder& order, const MarketEvent& event) noexcept;
    void match_trade(const MarketEvent& event) noexcept;
    void apply_due_cancels(const MarketEvent& event) noexcept;
    void fill(PaperOrder& order, std::int64_t quantity_units,
              std::int64_t price_units, bool maker,
              const MarketEvent& event) noexcept;
    [[nodiscard]] bool valid_config() const noexcept;

    PaperExchangeConfig config_;
    PerpetualLedger ledger_;
    RiskEngine risk_;
    std::optional<MarketEvent> market_;
    std::vector<PaperOrder> orders_;
    std::vector<PaperFill> fills_;
    std::unordered_map<std::string, std::size_t> order_index_;
    std::uint64_t next_order_id_{1};
    std::uint64_t next_fill_id_{1};
    std::uint64_t ignored_market_events_{0};
    bool config_valid_{false};
};

[[nodiscard]] std::string_view to_string(OrderStatus status) noexcept;

}  // namespace astra
