#include "astra/laya_gateway.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace astra {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

[[nodiscard]] const boost::json::object& object_member(const boost::json::object& object,
                                                       std::string_view key) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || !value->is_object()) {
        throw std::runtime_error("missing object: " + std::string(key));
    }
    return value->as_object();
}

[[nodiscard]] std::string string_member(const boost::json::object& object,
                                        std::string_view key) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || !value->is_string()) {
        throw std::runtime_error("missing string: " + std::string(key));
    }
    return value->as_string().c_str();
}

[[nodiscard]] std::uint32_t probability_ppm(const boost::json::object& probabilities,
                                            std::string_view key) {
    const auto* value = probabilities.if_contains(key);
    if (value == nullptr) return 0;
    double probability = 0.0;
    if (value->is_double()) probability = value->as_double();
    else if (value->is_int64()) probability = static_cast<double>(value->as_int64());
    else if (value->is_uint64()) probability = static_cast<double>(value->as_uint64());
    else throw std::runtime_error("invalid probability");
    if (!std::isfinite(probability) || probability < 0.0 || probability > 1.0) {
        throw std::runtime_error("probability outside [0,1]");
    }
    return static_cast<std::uint32_t>(std::llround(probability * 1'000'000.0));
}

}  // namespace

struct LayaGateway::Implementation {
    explicit Implementation(LayaGatewayConfig value)
        : config(std::move(value)), resolver(io), stream(io) {}

    void connect() {
        if (stream.socket().is_open()) return;
        const auto endpoints = resolver.resolve(config.host, config.port);
        stream.expires_after(config.timeout);
        stream.connect(endpoints);
    }

    void close() noexcept {
        beast::error_code error;
        stream.socket().shutdown(tcp::socket::shutdown_both, error);
        stream.socket().close(error);
    }

    LayaGatewayConfig config;
    asio::io_context io;
    tcp::resolver resolver;
    beast::tcp_stream stream;
};

LayaGateway::LayaGateway(LayaGatewayConfig config)
    : implementation_(std::make_unique<Implementation>(std::move(config))) {}
LayaGateway::~LayaGateway() = default;
LayaGateway::LayaGateway(LayaGateway&&) noexcept = default;
LayaGateway& LayaGateway::operator=(LayaGateway&&) noexcept = default;

std::string LayaGateway::request_json(const ModelState& state, std::string_view model) {
    boost::json::object market_state{
        {"schema", "astra.market-state.v1"},
        {"symbol", "BTCUSDT"},
        {"sequence", state.sequence},
        {"exchange_time_ns", state.exchange_time_ns},
        {"state_age_ns", state.state_age_ns},
        {"best_bid_units", state.best_bid_units},
        {"best_ask_units", state.best_ask_units},
        {"bid_quantity_units", state.bid_quantity_units},
        {"ask_quantity_units", state.ask_quantity_units},
        {"return_100_events_ppm", state.return_100_events_ppm},
        {"flow_imbalance_ppm", state.flow_imbalance_ppm},
        {"current_position_units", state.current_position_units},
    };
    boost::json::object criteria{
        {"long", "Price likely rises enough to exceed spread, fees, and slippage."},
        {"short", "Price likely falls enough to exceed spread, fees, and slippage."},
        {"hold", "No sufficiently reliable cost-adjusted directional edge."},
    };
    boost::json::object direction{
        {"type", "choice"},
        {"instructions",
         "Choose the cost-adjusted BTCUSDT action for the next short horizon. Prefer hold when "
         "the numeric market state is weak, conflicting, stale, or uncertain."},
        {"criteria", std::move(criteria)},
    };
    boost::json::object questions{{"direction", std::move(direction)}};
    boost::json::object request{
        {"model", model},
        {"state", std::move(market_state)},
        {"questions", std::move(questions)},
    };
    return boost::json::serialize(request);
}

ModelDecision LayaGateway::parse_response(std::string_view body,
                                          std::uint64_t state_sequence,
                                          std::int64_t latency_ns) {
    boost::json::error_code error;
    const auto parsed = boost::json::parse(body, error);
    if (error || !parsed.is_object()) throw std::runtime_error("invalid Laya JSON response");
    const auto& root = parsed.as_object();
    const auto& answer = object_member(object_member(root, "answers"), "direction");
    const auto& probabilities = object_member(answer, "probabilities");
    const auto choice = string_member(answer, "choice");
    ModelAction action = ModelAction::hold;
    if (choice == "long") action = ModelAction::long_position;
    else if (choice == "short") action = ModelAction::short_position;
    else if (choice != "hold") throw std::runtime_error("unknown Laya action");
    return {
        .action = action,
        .long_probability_ppm = probability_ppm(probabilities, "long"),
        .short_probability_ppm = probability_ppm(probabilities, "short"),
        .hold_probability_ppm = probability_ppm(probabilities, "hold"),
        .latency_ns = latency_ns,
        .state_sequence = state_sequence,
        .model = root.if_contains("model") != nullptr && root.at("model").is_string()
                     ? std::string(root.at("model").as_string().c_str())
                     : std::string{},
        .error = {},
    };
}

ModelDecision LayaGateway::decide(const ModelState& state) noexcept {
    const auto started = std::chrono::steady_clock::now();
    std::string last_error;
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            implementation_->connect();
            http::request<http::string_body> request{http::verb::post, "/v1/systemone", 11};
            request.set(http::field::host, implementation_->config.host);
            request.set(http::field::content_type, "application/json");
            request.set(http::field::user_agent, "astra-trader/0.1");
            request.keep_alive(true);
            request.body() = request_json(state, implementation_->config.model);
            request.prepare_payload();
            implementation_->stream.expires_after(implementation_->config.timeout);
            http::write(implementation_->stream, request);

            beast::flat_buffer buffer;
            http::response<http::string_body> response;
            implementation_->stream.expires_after(implementation_->config.timeout);
            http::read(implementation_->stream, buffer, response);
            if (response.result() != http::status::ok) {
                throw std::runtime_error("Laya HTTP status " +
                                         std::to_string(response.result_int()));
            }
            const auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            if (!response.keep_alive()) implementation_->close();
            return parse_response(response.body(), state.sequence, latency);
        } catch (const std::exception& error) {
            last_error = error.what();
            implementation_->close();
        }
    }
    return {.action = ModelAction::hold,
            .long_probability_ppm = 0,
            .short_probability_ppm = 0,
            .hold_probability_ppm = 1'000'000,
            .latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count(),
            .state_sequence = state.sequence,
            .model = implementation_->config.model,
            .error = std::move(last_error)};
}

void LayaGateway::close() noexcept { implementation_->close(); }

std::string_view to_string(ModelAction action) noexcept {
    switch (action) {
        case ModelAction::long_position: return "long";
        case ModelAction::short_position: return "short";
        case ModelAction::hold: return "hold";
    }
    return "hold";
}

}  // namespace astra
