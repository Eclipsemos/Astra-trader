#include "astra/audit_writer.hpp"
#include "astra/binance_public_feed.hpp"
#include "astra/exchange_clock.hpp"
#include "astra/feature_builder.hpp"
#include "astra/laya_gateway.hpp"
#include "astra/model_worker.hpp"
#include "astra/paper_exchange.hpp"
#include "astra/status_server.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

astra::BinancePublicFeed* active_feed = nullptr;

void stop_feed(int) {
    if (active_feed != nullptr) active_feed->stop();
}

astra::MarketEvent market_event(std::uint64_t sequence, std::int64_t time_ns,
                                std::int64_t bid, std::int64_t ask) {
    return {
        .sequence = sequence,
        .exchange_time_ns = time_ns,
        .receive_time_ns = time_ns + 1'000'000,
        .best_bid_units = bid,
        .best_ask_units = ask,
        .bid_quantity_units = 1'000,
        .ask_quantity_units = 1'000,
        .trade_price_units = std::nullopt,
        .trade_quantity_units = 0,
        .aggressor_side = std::nullopt,
    };
}

std::string status_json(const astra::MarketEvent& event,
                       const astra::PaperExchange& exchange,
                       const std::optional<astra::ModelDecision>& decision,
                       std::uint64_t coalesced_requests,
                       astra::ExchangeClockState clock,
                       const astra::AuditWriter& audit) {
    const auto& state = exchange.ledger().state();
    const auto action = decision.has_value() ? astra::to_string(decision->action) : "hold";
    const auto long_probability = decision.has_value() ? decision->long_probability_ppm : 0U;
    const auto short_probability = decision.has_value() ? decision->short_probability_ppm : 0U;
    const auto hold_probability = decision.has_value() ? decision->hold_probability_ppm : 1'000'000U;
    const auto latency = decision.has_value() ? decision->latency_ns : 0;
    return "{\"schema\":\"astra.status.v1\",\"status\":\"running\","
           "\"symbol\":\"BTCUSDT\",\"sequence\":" + std::to_string(event.sequence) +
           ",\"exchange_time_ns\":" + std::to_string(event.exchange_time_ns) +
           ",\"receive_time_ns\":" + std::to_string(event.receive_time_ns) +
           ",\"best_bid_units\":" + std::to_string(event.best_bid_units) +
           ",\"best_ask_units\":" + std::to_string(event.best_ask_units) +
           ",\"position_units\":" + std::to_string(state.position_units) +
           ",\"equity_units\":" + std::to_string(state.equity_units) +
           ",\"realized_pnl_units\":" + std::to_string(state.realized_pnl_units) +
           ",\"unrealized_pnl_units\":" + std::to_string(state.unrealized_pnl_units) +
           ",\"fees_units\":" + std::to_string(state.total_fee_units) +
           ",\"orders\":" + std::to_string(exchange.orders().size()) +
           ",\"fills\":" + std::to_string(exchange.fills().size()) +
           ",\"model_action\":\"" + std::string(action) +
           "\",\"long_probability_ppm\":" + std::to_string(long_probability) +
           ",\"short_probability_ppm\":" + std::to_string(short_probability) +
           ",\"hold_probability_ppm\":" + std::to_string(hold_probability) +
           ",\"model_latency_ns\":" + std::to_string(latency) +
           ",\"clock_offset_ns\":" + std::to_string(clock.offset_ns) +
           ",\"adjusted_data_age_ns\":" + std::to_string(clock.adjusted_age_ns) +
           ",\"coalesced_model_requests\":" + std::to_string(coalesced_requests) +
           ",\"trading_ready\":" + (clock.stable ? "true" : "false") +
           ",\"audit_healthy\":" + (audit.healthy() ? "true" : "false") +
           ",\"audit_dropped_events\":" + std::to_string(audit.dropped_events()) + "}";
}

std::string decision_audit_json(const astra::ModelDecision& decision) {
    return "{\"schema\":\"astra.audit.v1\",\"event\":\"decision\",\"sequence\":" +
           std::to_string(decision.state_sequence) + ",\"action\":\"" +
           std::string(astra::to_string(decision.action)) +
           "\",\"long_probability_ppm\":" +
           std::to_string(decision.long_probability_ppm) +
           ",\"short_probability_ppm\":" + std::to_string(decision.short_probability_ppm) +
           ",\"hold_probability_ppm\":" + std::to_string(decision.hold_probability_ppm) +
           ",\"latency_ns\":" + std::to_string(decision.latency_ns) +
           ",\"succeeded\":" + (decision.succeeded() ? "true" : "false") + "}";
}

}  // namespace

int main(int argc, char** argv) {
    using namespace astra;
    bool live = false;
    bool use_laya = true;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--live") live = true;
        else if (argument == "--baseline") use_laya = false;
        else {
            std::cerr << "usage: astra_trader [--live [--baseline]]\n";
            return 64;
        }
    }
    if (!live && !use_laya) {
        std::cerr << "--baseline requires --live\n";
        return 64;
    }

    PaperExchange exchange({
        .ledger = {.initial_cash_units = 1'000'000,
                   .account_scale = 2,
                   .price_scale = 2,
                   .quantity_scale = 3},
        .risk = {.maximum_order_quantity_units = 10,
                 .maximum_order_notional_units = 25'000,
                 .maximum_absolute_position_units = 10,
                 .maximum_data_age_ns = 2'000'000'000,
                 .maximum_spread_ppm = 10'000,
                 .maximum_drawdown_units = 100'000},
        .quantity_step_units = 1,
        .minimum_notional_units = 100,
        .maker_fee_ppm = 200,
        .taker_fee_ppm = 500,
        .market_slippage_ppm = 100,
        .acknowledgement_latency_ns = 0,
        .cancellation_latency_ns = 5'000'000,
    });

    if (live) {
        std::signal(SIGINT, stop_feed);
        std::signal(SIGTERM, stop_feed);
        BinancePublicFeed feed;
        LayaGateway laya({.host = "127.0.0.1",
                          .port = "8000",
                          .model = "english",
                          .timeout = std::chrono::milliseconds(250)});
        auto status_store = std::make_shared<StatusStore>();
        StatusServer status_server(status_store, 8765);
        AuditWriter audit("data/paper-events.jsonl");
        FeatureBuilder feature_builder(100);
        ExchangeClock exchange_clock;
        std::optional<ModelDecision> latest_decision;
        std::uint64_t last_decision_sequence = 0;
        std::unique_ptr<ModelWorker> model_worker;
        if (use_laya) {
            model_worker = std::make_unique<ModelWorker>([&laya](const ModelState& state) {
                return laya.decide(state);
            });
        }
        active_feed = &feed;
        std::uint64_t decisions = 0;
        feed.run(
            [&](const MarketEvent& event) {
                if (!exchange.on_market_event(event)) return;
                const auto position = exchange.ledger().state().position_units;
                const auto clock = exchange_clock.observe(event.exchange_time_ns,
                                                          event.receive_time_ns);
                auto state = feature_builder.update(event, position);
                state.state_age_ns = clock.adjusted_age_ns;
                if (use_laya && event.sequence % 1'000U == 0U) {
                    model_worker->submit(state);
                }
                if (!use_laya && event.sequence % 500U == 0U) {
                    const auto side = event.bid_quantity_units >= event.ask_quantity_units
                                          ? ModelAction::long_position
                                          : ModelAction::short_position;
                    latest_decision = ModelDecision{
                        .action = side,
                        .long_probability_ppm = side == ModelAction::long_position ? 600'000U : 100'000U,
                        .short_probability_ppm = side == ModelAction::short_position ? 600'000U : 100'000U,
                        .hold_probability_ppm = 300'000,
                        .latency_ns = 0,
                        .state_sequence = event.sequence,
                        .model = "baseline",
                        .error = {}};
                    audit.append(decision_audit_json(*latest_decision));
                }
                if (use_laya) {
                    if (auto completed = model_worker->try_take(); completed.has_value()) {
                        audit.append(decision_audit_json(*completed));
                        latest_decision = std::move(completed);
                    }
                }
                if (event.sequence % 100U == 0U) {
                    status_store->publish(event.sequence,
                                          status_json(event, exchange, latest_decision,
                                                      model_worker != nullptr
                                                          ? model_worker->coalesced_requests()
                                                          : 0,
                                                      clock,
                                                      audit));
                }
                if (!latest_decision.has_value() ||
                    latest_decision->state_sequence <= last_decision_sequence ||
                    latest_decision->state_sequence > event.sequence ||
                    event.sequence - latest_decision->state_sequence > 2'500U) {
                    return;
                }
                ++decisions;
                last_decision_sequence = latest_decision->state_sequence;
                const auto& model_decision = *latest_decision;
                const auto probability = model_decision.action == ModelAction::long_position
                                             ? model_decision.long_probability_ppm
                                             : model_decision.action == ModelAction::short_position
                                                   ? model_decision.short_probability_ppm
                                                   : model_decision.hold_probability_ppm;
                const bool permitted = model_decision.succeeded() &&
                                       model_decision.action != ModelAction::hold &&
                                       probability >= 550'000U && clock.stable;
                if (!permitted) return;
                const auto signal = model_decision.action == ModelAction::long_position
                                        ? Side::buy
                                        : Side::sell;
                const auto model_latency_ns = model_decision.latency_ns;
                const auto model_name = model_decision.model.empty() ? "laya" : model_decision.model;
                const auto wanted = signal == Side::buy ? 1 : -1;
                const auto delta = wanted - position;
                if (delta == 0) return;
                const auto quantity = delta > 0 ? delta : -delta;
                const auto request = OrderRequest{
                    .client_order_id = "paper-live-" + std::to_string(event.sequence),
                    .side = delta > 0 ? Side::buy : Side::sell,
                    .type = OrderType::market,
                    .quantity_units = quantity,
                    .limit_price_units = std::nullopt,
                    .created_time_ns = event.receive_time_ns,
                    .reduce_only = false,
                };
                const auto result = exchange.submit(request, event.receive_time_ns);
                audit.append("{\"schema\":\"astra.audit.v1\",\"event\":\"order\","
                             "\"sequence\":" + std::to_string(event.sequence) +
                             ",\"order_id\":" + std::to_string(result.order_id) +
                             ",\"status\":\"" + std::string(to_string(result.status)) +
                             "\",\"position_units\":" +
                             std::to_string(exchange.ledger().state().position_units) + "}");
                std::cout << "{\"mode\":\"paper-live\",\"model\":\""
                          << model_name << "\",\"sequence\":"
                          << event.sequence << ",\"decision\":" << decisions
                          << ",\"signal\":\""
                          << (signal == Side::buy ? "long" : "short")
                          << "\",\"order_status\":\"" << to_string(result.status)
                          << "\",\"fills\":" << exchange.fills().size()
                          << ",\"position_units\":"
                          << exchange.ledger().state().position_units
                          << ",\"equity_units\":"
                          << exchange.ledger().state().equity_units
                          << ",\"model_latency_ns\":" << model_latency_ns << "}\n";
            },
            [](std::string error) {
                std::cerr << "paper feed error: " << error << '\n';
            });
        active_feed = nullptr;
        status_server.stop();
        std::cout << "paper-live stopped; market_events="
                  << (exchange.market().has_value() ? exchange.market()->sequence : 0)
                  << " decisions=" << decisions
                  << " fills=" << exchange.fills().size()
                  << " equity_units=" << exchange.ledger().state().equity_units << '\n';
        return 0;
    }

    PaperExchange synthetic_exchange({
        .ledger = {.initial_cash_units = 1'000'000, .account_scale = 2,
                   .price_scale = 2, .quantity_scale = 0},
        .risk = {.maximum_order_quantity_units = 100,
                 .maximum_order_notional_units = 500'000,
                 .maximum_absolute_position_units = 100,
                 .maximum_data_age_ns = 100'000'000,
                 .maximum_spread_ppm = 20'000,
                 .maximum_drawdown_units = 100'000},
        .quantity_step_units = 1,
        .minimum_notional_units = 100,
        .maker_fee_ppm = 200,
        .taker_fee_ppm = 500,
        .market_slippage_ppm = 100,
        .acknowledgement_latency_ns = 0,
        .cancellation_latency_ns = 5'000'000,
    });

    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto first = market_event(1, now, 10'000, 10'001);
    first.bid_quantity_units = 0;
    if (!synthetic_exchange.on_market_event(first)) return 1;

    const auto submitted = synthetic_exchange.submit({
        .client_order_id = "paper-1",
        .side = Side::buy,
        .type = OrderType::post_only,
        .quantity_units = 10,
        .limit_price_units = 10'000,
        .created_time_ns = now,
    }, first.receive_time_ns);
    if (submitted.status == OrderStatus::rejected) return 2;

    auto trade = market_event(2, now + 10'000'000, 9'999, 10'000);
    trade.trade_price_units = 10'000;
    trade.trade_quantity_units = 20;
    trade.aggressor_side = Side::sell;
    if (!synthetic_exchange.on_market_event(trade)) return 3;

    const auto& state = synthetic_exchange.ledger().state();
    std::cout << "{\"mode\":\"paper\",\"orders\":" << synthetic_exchange.orders().size()
              << ",\"fills\":" << synthetic_exchange.fills().size()
              << ",\"position_units\":" << state.position_units
              << ",\"equity_units\":" << state.equity_units
              << ",\"status\":\"running\"}\n";
    return synthetic_exchange.fills().size() == 1U && state.position_units == 10 ? 0 : 4;
}
