#include "astra/feature_builder.hpp"
#include "astra/laya_gateway.hpp"
#include "astra/paper_exchange.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using astra::MarketEvent;

#if defined(__SIZEOF_INT128__)
__extension__ using Wide = __int128;
#endif

struct Options {
    std::string input;
    std::string output;
    std::string detail;
    std::string model{"baseline"};
    std::string decision_tape;
    std::string laya_host{"127.0.0.1"};
    std::string laya_port{"8000"};
    std::uint64_t decision_every{1'000};
    std::int64_t start_ns{0};
    std::int64_t end_ns{std::numeric_limits<std::int64_t>::max()};
    std::int64_t feed_latency_ns{1'000'000};
    std::uint32_t probability_threshold_ppm{550'000};
    std::uint32_t taker_fee_ppm{500};
    std::uint32_t market_slippage_ppm{100};
    std::int64_t laya_timeout_ms{250};
};

struct ReplayRow {
    MarketEvent event;
};

struct PendingDecision {
    astra::ModelDecision decision;
    std::int64_t decision_time_ns{0};
};

struct Metrics {
    std::uint64_t events{0};
    std::uint64_t decisions{0};
    std::uint64_t signals{0};
    std::uint64_t accepted_orders{0};
    std::uint64_t rejected_orders{0};
    std::uint64_t fills{0};
    std::uint64_t ignored_events{0};
    std::int64_t initial_equity{0};
    std::int64_t final_equity{0};
    std::int64_t peak_equity{0};
    std::int64_t max_drawdown{0};
    std::int64_t realized_pnl{0};
    std::int64_t unrealized_pnl{0};
    std::int64_t fees{0};
    std::int64_t gross_profit{0};
    std::int64_t gross_loss{0};
};

void usage() {
    std::cerr << "usage: astra_replay --input FILE --output FILE "
                 "[--detail FILE] [--model baseline|hold|laya|tape] "
                 "[--decision-tape FILE] "
                 "[--decision-every N] [--start-ns N] [--end-ns N] "
                 "[--feed-latency-ns N] [--probability-threshold-ppm N] "
                 "[--taker-fee-ppm N] [--market-slippage-ppm N] "
                 "[--laya-host HOST] [--laya-port PORT] [--laya-timeout-ms N]\n";
}

template <typename T>
bool parse_integer(std::string_view value, T& result) {
    if (value.empty()) return false;
    T parsed{};
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto [next, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || next != end) return false;
    result = parsed;
    return true;
}

bool option_value(int& index, int argc, char** argv, std::string_view name,
                  std::string_view& value) {
    if (std::string_view(argv[index]) != name || index + 1 >= argc) return false;
    value = argv[++index];
    return true;
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        std::string_view value;
        if (option_value(index, argc, argv, "--input", value)) options.input = value;
        else if (option_value(index, argc, argv, "--output", value)) options.output = value;
        else if (option_value(index, argc, argv, "--detail", value)) options.detail = value;
        else if (option_value(index, argc, argv, "--model", value)) options.model = value;
        else if (option_value(index, argc, argv, "--decision-tape", value)) options.decision_tape = value;
        else if (option_value(index, argc, argv, "--laya-host", value)) options.laya_host = value;
        else if (option_value(index, argc, argv, "--laya-port", value)) options.laya_port = value;
        else if (option_value(index, argc, argv, "--decision-every", value) &&
                 parse_integer(value, options.decision_every)) {}
        else if (option_value(index, argc, argv, "--start-ns", value) &&
                 parse_integer(value, options.start_ns)) {}
        else if (option_value(index, argc, argv, "--end-ns", value) &&
                 parse_integer(value, options.end_ns)) {}
        else if (option_value(index, argc, argv, "--feed-latency-ns", value) &&
                 parse_integer(value, options.feed_latency_ns)) {}
        else if (option_value(index, argc, argv, "--probability-threshold-ppm", value) &&
                 parse_integer(value, options.probability_threshold_ppm)) {}
        else if (option_value(index, argc, argv, "--taker-fee-ppm", value) &&
                 parse_integer(value, options.taker_fee_ppm)) {}
        else if (option_value(index, argc, argv, "--market-slippage-ppm", value) &&
                 parse_integer(value, options.market_slippage_ppm)) {}
        else if (option_value(index, argc, argv, "--laya-timeout-ms", value) &&
                 parse_integer(value, options.laya_timeout_ms)) {}
        else {
            return false;
        }
    }
    return !options.input.empty() && !options.output.empty() && options.decision_every > 0 &&
           (options.model == "baseline" || options.model == "hold" || options.model == "laya" ||
            options.model == "tape") &&
           (options.model != "tape" || !options.decision_tape.empty()) &&
           options.feed_latency_ns >= 0 && options.laya_timeout_ms > 0 &&
           options.taker_fee_ppm <= 1'000'000 && options.market_slippage_ppm <= 1'000'000;
}

bool split_csv(std::string_view line, std::vector<std::string_view>& fields) {
    fields.clear();
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const auto end = line.find(',', begin);
        if (end == std::string_view::npos) {
            fields.push_back(line.substr(begin));
            return true;
        }
        fields.push_back(line.substr(begin, end - begin));
        begin = end + 1;
    }
    return false;
}

bool parse_row(std::string_view line, std::int64_t feed_latency_ns, ReplayRow& row) {
    std::vector<std::string_view> fields;
    if (!split_csv(line, fields) || fields.size() != 9U) return false;
    std::uint64_t sequence{};
    std::int64_t exchange_time_ns{};
    std::int64_t best_bid{};
    std::int64_t best_ask{};
    std::int64_t bid_quantity{};
    std::int64_t ask_quantity{};
    std::int64_t trade_price{};
    std::int64_t trade_quantity{};
    if (!parse_integer(fields[0], sequence) || !parse_integer(fields[1], exchange_time_ns) ||
        !parse_integer(fields[2], best_bid) || !parse_integer(fields[3], best_ask) ||
        !parse_integer(fields[4], bid_quantity) || !parse_integer(fields[5], ask_quantity) ||
        !parse_integer(fields[6], trade_price) || !parse_integer(fields[7], trade_quantity) ||
        sequence == 0 || exchange_time_ns <= 0 || best_bid <= 0 || best_ask < best_bid ||
        bid_quantity < 0 || ask_quantity < 0 || trade_price < 0 || trade_quantity < 0) {
        return false;
    }
    std::optional<astra::Side> aggressor;
    if (fields[8] == "buy") aggressor = astra::Side::buy;
    else if (fields[8] == "sell") aggressor = astra::Side::sell;
    else if (fields[8] != "none") return false;
    row.event = {
        .sequence = sequence,
        .exchange_time_ns = exchange_time_ns,
        .receive_time_ns = exchange_time_ns + feed_latency_ns,
        .best_bid_units = best_bid,
        .best_ask_units = best_ask,
        .bid_quantity_units = bid_quantity,
        .ask_quantity_units = ask_quantity,
        .trade_price_units = trade_price > 0 ? std::optional(trade_price) : std::nullopt,
        .trade_quantity_units = trade_quantity,
        .aggressor_side = aggressor,
    };
    return true;
}

std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        if (character == '"' || character == '\\') escaped.push_back('\\');
        escaped.push_back(character);
    }
    return escaped;
}

astra::ModelDecision baseline_decision(const astra::ModelState& state) {
    constexpr std::int64_t threshold = 50'000;
    if (state.flow_imbalance_ppm > threshold) {
        return {.action = astra::ModelAction::long_position,
                .long_probability_ppm = 700'000,
                .short_probability_ppm = 100'000,
                .hold_probability_ppm = 200'000,
                .state_sequence = state.sequence,
                .model = "flow-baseline",
                .error = {}};
    }
    if (state.flow_imbalance_ppm < -threshold) {
        return {.action = astra::ModelAction::short_position,
                .long_probability_ppm = 100'000,
                .short_probability_ppm = 700'000,
                .hold_probability_ppm = 200'000,
                .state_sequence = state.sequence,
                .model = "flow-baseline",
                .error = {}};
    }
    return {.action = astra::ModelAction::hold,
            .long_probability_ppm = 100'000,
            .short_probability_ppm = 100'000,
            .hold_probability_ppm = 800'000,
            .state_sequence = state.sequence,
            .model = "flow-baseline",
            .error = {}};
}

astra::ModelDecision hold_decision(std::uint64_t sequence) {
    return {.action = astra::ModelAction::hold,
            .long_probability_ppm = 0,
            .short_probability_ppm = 0,
            .hold_probability_ppm = 1'000'000,
            .state_sequence = sequence,
            .model = "hold",
            .error = {}};
}

class DecisionTape {
public:
    bool load(const std::string& path, std::string& error) {
        std::ifstream input(path);
        if (!input) {
            error = "cannot open decision tape: " + path;
            return false;
        }
        std::string line;
        std::uint64_t row_number = 0;
        while (std::getline(input, line)) {
            ++row_number;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line == "sequence,action,long_probability_ppm,short_probability_ppm,hold_probability_ppm") {
                continue;
            }
            std::vector<std::string_view> fields;
            if (!split_csv(line, fields) || fields.size() != 5U) {
                error = "invalid decision tape row " + std::to_string(row_number);
                return false;
            }
            std::uint64_t sequence{};
            std::uint32_t long_probability{};
            std::uint32_t short_probability{};
            std::uint32_t hold_probability{};
            if (!parse_integer(fields[0], sequence) || !parse_integer(fields[2], long_probability) ||
                !parse_integer(fields[3], short_probability) || !parse_integer(fields[4], hold_probability) ||
                sequence == 0 || long_probability > 1'000'000 || short_probability > 1'000'000 ||
                hold_probability > 1'000'000) {
                error = "invalid decision tape values at row " + std::to_string(row_number);
                return false;
            }
            astra::ModelAction action;
            if (fields[1] == "long") action = astra::ModelAction::long_position;
            else if (fields[1] == "short") action = astra::ModelAction::short_position;
            else if (fields[1] == "hold") action = astra::ModelAction::hold;
            else {
                error = "invalid decision tape action at row " + std::to_string(row_number);
                return false;
            }
            decisions_[sequence] = {.action = action,
                                    .long_probability_ppm = long_probability,
                                    .short_probability_ppm = short_probability,
                                    .hold_probability_ppm = hold_probability,
                                    .state_sequence = sequence,
                                    .model = "decision-tape",
                                    .error = {}};
        }
        if (decisions_.empty()) {
            error = "decision tape is empty";
            return false;
        }
        return true;
    }

    astra::ModelDecision decide(std::uint64_t sequence) const {
        const auto found = decisions_.find(sequence);
        if (found != decisions_.end()) return found->second;
        auto decision = hold_decision(sequence);
        decision.model = "decision-tape";
        decision.error = "decision tape has no decision for sequence " + std::to_string(sequence);
        return decision;
    }

    [[nodiscard]] bool contains(std::uint64_t sequence) const {
        return decisions_.find(sequence) != decisions_.end();
    }

private:
    std::unordered_map<std::uint64_t, astra::ModelDecision> decisions_;
};

void write_fill_detail(std::ofstream& detail, const astra::PaperFill& fill) {
    detail << "{\"event\":\"fill\",\"fill_id\":" << fill.fill_id
           << ",\"order_id\":" << fill.order_id << ",\"sequence\":"
           << fill.market_sequence << ",\"side\":\""
           << (fill.side == astra::Side::buy ? "buy" : "sell") << "\",\"quantity_units\":"
           << fill.quantity_units << ",\"price_units\":" << fill.price_units
           << ",\"fee_units\":" << fill.fee_units << ",\"maker\":"
           << (fill.maker ? "true" : "false") << "}\n";
}

void write_decision_detail(std::ofstream& detail, const astra::ModelDecision& decision,
                           std::string_view proposed_action, bool gate_hold) {
    detail << "{\"event\":\"decision\",\"sequence\":" << decision.state_sequence
           << ",\"action\":\"" << astra::to_string(decision.action)
           << "\",\"proposed_action\":\"" << proposed_action
           << "\",\"gate_hold\":" << (gate_hold ? "true" : "false")
           << ",\"long_probability_ppm\":" << decision.long_probability_ppm
           << ",\"short_probability_ppm\":" << decision.short_probability_ppm
           << ",\"hold_probability_ppm\":" << decision.hold_probability_ppm
           << ",\"latency_ns\":" << decision.latency_ns << ",\"model\":\""
           << json_escape(decision.model) << "\",\"succeeded\":"
           << (decision.succeeded() ? "true" : "false") << "}\n";
}

std::string metrics_json(const Options& options, const Metrics& metrics,
                         const astra::PaperExchange& exchange) {
    const auto initial = metrics.initial_equity == 0 ? 1 : metrics.initial_equity;
    const auto pnl = metrics.final_equity - metrics.initial_equity;
    const auto return_ppm = static_cast<Wide>(pnl) * 1'000'000 / initial;
    const auto fixed_ratio = [](std::int64_t numerator, std::int64_t denominator) {
        if (denominator <= 0) return numerator > 0 ? std::string("inf") : std::string("0.000");
        const auto scaled = static_cast<Wide>(numerator) * 1000 / denominator;
        const auto whole = scaled / 1000;
        const auto fractional = static_cast<std::int64_t>(scaled % 1000);
        return std::to_string(static_cast<std::int64_t>(whole)) + "." +
               (fractional < 100 ? fractional < 10 ? "00" : "0" : "") +
               std::to_string(fractional);
    };
    return "{\"schema\":\"astra.replay.metrics.v1\",\"model\":\"" +
           json_escape(options.model) + "\",\"input\":\"" + json_escape(options.input) +
           "\",\"decision_every\":" + std::to_string(options.decision_every) +
           ",\"taker_fee_ppm\":" + std::to_string(options.taker_fee_ppm) +
           ",\"market_slippage_ppm\":" + std::to_string(options.market_slippage_ppm) +
           ",\"start_ns\":" + std::to_string(options.start_ns) +
           ",\"end_ns\":" + std::to_string(options.end_ns) + ",\"events\":" +
           std::to_string(metrics.events) + ",\"decisions\":" +
           std::to_string(metrics.decisions) + ",\"signals\":" +
           std::to_string(metrics.signals) + ",\"accepted_orders\":" +
           std::to_string(metrics.accepted_orders) + ",\"rejected_orders\":" +
           std::to_string(metrics.rejected_orders) + ",\"fills\":" +
           std::to_string(metrics.fills) + ",\"ignored_events\":" +
           std::to_string(metrics.ignored_events) + ",\"initial_equity_units\":" +
           std::to_string(metrics.initial_equity) + ",\"final_equity_units\":" +
           std::to_string(metrics.final_equity) + ",\"pnl_units\":" +
           std::to_string(pnl) + ",\"return_ppm\":" + [](Wide value) {
               if (value == 0) return std::string("0");
               const bool negative = value < 0;
               if (negative) value = -value;
               std::string digits;
               while (value > 0) {
                   digits.push_back(static_cast<char>('0' + value % 10));
                   value /= 10;
               }
               if (negative) digits.push_back('-');
               std::reverse(digits.begin(), digits.end());
               return digits;
           }(return_ppm) +
           ",\"max_drawdown_units\":" + std::to_string(metrics.max_drawdown) +
           ",\"realized_pnl_units\":" + std::to_string(metrics.realized_pnl) +
           ",\"unrealized_pnl_units\":" + std::to_string(metrics.unrealized_pnl) +
           ",\"fees_units\":" + std::to_string(metrics.fees) +
           ",\"gross_profit_units\":" + std::to_string(metrics.gross_profit) +
           ",\"gross_loss_units\":" + std::to_string(metrics.gross_loss) +
           ",\"profit_factor\":\"" + fixed_ratio(metrics.gross_profit, metrics.gross_loss) + "\"" +
           ",\"position_units\":" +
           std::to_string(exchange.ledger().state().position_units) + "}\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        usage();
        return 64;
    }

    std::ifstream input(options.input);
    if (!input) {
        std::cerr << "cannot open input: " << options.input << '\n';
        return 66;
    }
    std::ofstream output(options.output);
    if (!output) {
        std::cerr << "cannot open output: " << options.output << '\n';
        return 73;
    }
    std::ofstream detail;
    if (!options.detail.empty()) {
        detail.open(options.detail);
        if (!detail) {
            std::cerr << "cannot open detail output: " << options.detail << '\n';
            return 73;
        }
    }

    std::optional<DecisionTape> decision_tape;
    if (options.model == "tape") {
        decision_tape.emplace();
        std::string tape_error;
        if (!decision_tape->load(options.decision_tape, tape_error)) {
            std::cerr << tape_error << '\n';
            return 65;
        }
    }

    astra::PaperExchange exchange({
        .ledger = {.initial_cash_units = 10'000'000,
                   .account_scale = 2,
                   .price_scale = 2,
                   .quantity_scale = 3},
        .risk = {.maximum_order_quantity_units = 200,
                 .maximum_order_notional_units = 10'000'000,
                 .maximum_absolute_position_units = 100,
                 .maximum_data_age_ns = 120'000'000'000,
                 .maximum_spread_ppm = 50'000,
                 .maximum_drawdown_units = 5'000'000},
        .quantity_step_units = 1,
        .minimum_notional_units = 100,
        .maker_fee_ppm = 0,
        .taker_fee_ppm = options.taker_fee_ppm,
        .market_slippage_ppm = options.market_slippage_ppm,
        .acknowledgement_latency_ns = 1,
        .cancellation_latency_ns = 0,
    });
    std::optional<astra::LayaGateway> laya;
    if (options.model == "laya") {
        laya.emplace(astra::LayaGatewayConfig{.host = options.laya_host,
                                              .port = options.laya_port,
                                              .model = "english",
                                              .timeout = std::chrono::milliseconds(
                                                  options.laya_timeout_ms)});
    }

    astra::FeatureBuilder features(100);
    Metrics metrics;
    metrics.initial_equity = exchange.ledger().state().equity_units;
    metrics.peak_equity = metrics.initial_equity;
    std::optional<PendingDecision> pending_decision;
    std::uint64_t last_fill_count = 0;
    std::string line;
    bool header = true;
    std::uint64_t row_number = 0;

    while (std::getline(input, line)) {
        if (header) {
            header = false;
            if (line == "sequence,exchange_time_ns,best_bid_units,best_ask_units,bid_quantity_units,ask_quantity_units,trade_price_units,trade_quantity_units,aggressor_side") {
                continue;
            }
        }
        ++row_number;
        ReplayRow row;
        if (!parse_row(line, options.feed_latency_ns, row)) {
            std::cerr << "invalid replay row " << row_number << '\n';
            return 65;
        }
        const auto& event = row.event;
        if (event.exchange_time_ns < options.start_ns || event.exchange_time_ns > options.end_ns) {
            continue;
        }

        if (pending_decision.has_value() && pending_decision->decision.succeeded()) {
            const auto action = pending_decision->decision.action;
            const auto position = exchange.ledger().state().position_units;
            constexpr std::int64_t target_units = 100;
            const auto target = action == astra::ModelAction::long_position
                                    ? target_units
                                    : action == astra::ModelAction::short_position
                                          ? -target_units
                                          : position;
            const auto delta = target - position;
            if (delta != 0) {
                ++metrics.signals;
                const auto request = astra::OrderRequest{
                    .client_order_id = "replay-" +
                                       std::to_string(pending_decision->decision.state_sequence),
                    .side = delta > 0 ? astra::Side::buy : astra::Side::sell,
                    .type = astra::OrderType::market,
                    .quantity_units = delta > 0 ? delta : -delta,
                    .limit_price_units = std::nullopt,
                    .created_time_ns = pending_decision->decision_time_ns,
                    .reduce_only = false,
                };
                const auto result = exchange.submit(request, pending_decision->decision_time_ns);
                if (result.status == astra::OrderStatus::rejected) ++metrics.rejected_orders;
                else ++metrics.accepted_orders;
            }
        }
        pending_decision.reset();

        if (!exchange.on_market_event(event)) {
            ++metrics.ignored_events;
            continue;
        }
        ++metrics.events;
        const auto state = features.update(event, exchange.ledger().state().position_units);
        const bool tape_decision = options.model == "tape" && decision_tape.has_value() &&
                                   decision_tape->contains(event.sequence);
        if (event.sequence % options.decision_every == 0 || tape_decision) {
            astra::ModelDecision decision;
            if (options.model == "baseline") decision = baseline_decision(state);
            else if (options.model == "hold") decision = hold_decision(state.sequence);
            else if (options.model == "laya") decision = laya->decide(state);
            else decision = decision_tape->decide(state.sequence);
            ++metrics.decisions;
            const auto proposed_action = astra::to_string(decision.action);
            const auto probability = decision.action == astra::ModelAction::long_position
                                         ? decision.long_probability_ppm
                                         : decision.action == astra::ModelAction::short_position
                                               ? decision.short_probability_ppm
                                               : decision.hold_probability_ppm;
            const bool gate_hold = probability < options.probability_threshold_ppm;
            if (gate_hold) decision.action = astra::ModelAction::hold;
            if (detail) write_decision_detail(detail, decision, proposed_action, gate_hold);
            pending_decision = PendingDecision{.decision = std::move(decision),
                                               .decision_time_ns = event.receive_time_ns};
        }

        const auto& state_after = exchange.ledger().state();
        metrics.peak_equity = std::max(metrics.peak_equity, state_after.equity_units);
        metrics.max_drawdown = std::max(metrics.max_drawdown,
                                        metrics.peak_equity - state_after.equity_units);
        if (detail) {
            while (last_fill_count < exchange.fills().size()) {
                write_fill_detail(detail, exchange.fills()[last_fill_count]);
                ++last_fill_count;
            }
        }
    }

    metrics.final_equity = exchange.ledger().state().equity_units;
    metrics.realized_pnl = exchange.ledger().state().realized_pnl_units;
    metrics.unrealized_pnl = exchange.ledger().state().unrealized_pnl_units;
    metrics.fees = exchange.ledger().state().total_fee_units;
    metrics.fills = exchange.fills().size();
    for (const auto& fill : exchange.fills()) {
        if (fill.realized_pnl_units > 0) metrics.gross_profit += fill.realized_pnl_units;
        if (fill.realized_pnl_units < 0) metrics.gross_loss -= fill.realized_pnl_units;
    }
    output << metrics_json(options, metrics, exchange);
    std::cout << metrics_json(options, metrics, exchange);
    return 0;
}
