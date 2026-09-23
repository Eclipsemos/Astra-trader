#include "astra/binance_public_feed.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <boost/json/src.hpp>
#include <openssl/ssl.h>

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace astra {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using SslWebSocket = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

struct ProxyAddress {
    std::string host;
    std::string port;
};

[[nodiscard]] std::optional<ProxyAddress> proxy_from_environment() {
    const char* raw = std::getenv("HTTPS_PROXY");
    if (raw == nullptr || *raw == '\0') raw = std::getenv("https_proxy");
    if (raw == nullptr || *raw == '\0') return std::nullopt;
    std::string_view value(raw);
    constexpr std::string_view prefix = "http://";
    if (value.starts_with(prefix)) value.remove_prefix(prefix.size());
    const auto colon = value.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= value.size()) {
        throw std::runtime_error("HTTPS_PROXY must be http://host:port");
    }
    return ProxyAddress{std::string(value.substr(0, colon)),
                        std::string(value.substr(colon + 1))};
}

void establish_proxy_tunnel(beast::tcp_stream& stream, const BinancePublicFeedConfig& config) {
    namespace http = beast::http;
    http::request<http::empty_body> request{
        http::verb::connect, config.host + ":" + config.port, 11};
    request.set(http::field::host, config.host + ":" + config.port);
    request.set(http::field::user_agent, "astra-trader/0.1");
    http::write(stream, request);
    beast::flat_buffer response_buffer;
    http::response_parser<http::empty_body> parser;
    parser.skip(true);
    http::read_header(stream, response_buffer, parser);
    const auto& response = parser.get();
    if (response.result() != http::status::ok) {
        throw std::runtime_error("HTTPS proxy CONNECT failed: " +
                                 std::to_string(response.result_int()));
    }
}

[[nodiscard]] std::int64_t parse_scaled(std::string_view text, std::uint8_t scale) {
    if (text.empty()) throw std::runtime_error("empty decimal");
    bool negative = false;
    if (text.front() == '-' || text.front() == '+') {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    const auto point = text.find('.');
    const auto whole = text.substr(0, point);
    const auto fraction = point == std::string_view::npos
                              ? std::string_view{}
                              : text.substr(point + 1);
    if (whole.empty() || fraction.size() > scale) throw std::runtime_error("invalid decimal");
    std::uint64_t whole_value = 0;
    const auto whole_result = std::from_chars(whole.data(), whole.data() + whole.size(), whole_value);
    if (whole_result.ec != std::errc{} || whole_result.ptr != whole.data() + whole.size()) {
        throw std::runtime_error("invalid decimal whole");
    }
    std::uint64_t fraction_value = 0;
    if (!fraction.empty()) {
        const auto fraction_result = std::from_chars(
            fraction.data(), fraction.data() + fraction.size(), fraction_value);
        if (fraction_result.ec != std::errc{} ||
            fraction_result.ptr != fraction.data() + fraction.size()) {
            throw std::runtime_error("invalid decimal fraction");
        }
    }
    std::int64_t scale_factor = 1;
    for (std::uint8_t index = 0; index < scale; ++index) scale_factor *= 10;
    for (std::size_t index = fraction.size(); index < scale; ++index) fraction_value *= 10;
    const auto value = static_cast<std::int64_t>(whole_value) * scale_factor +
                       static_cast<std::int64_t>(fraction_value);
    return negative ? -value : value;
}

[[nodiscard]] std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] const boost::json::value& member(const boost::json::object& value,
                                               const char* key) {
    const auto* found = value.if_contains(key);
    if (found == nullptr) throw std::runtime_error(std::string("missing ") + key);
    return *found;
}

[[nodiscard]] std::string string_member(const boost::json::object& value, const char* key) {
    const auto& item = member(value, key);
    if (!item.is_string()) throw std::runtime_error(std::string("non-string ") + key);
    return item.as_string().c_str();
}

[[nodiscard]] std::int64_t int_member(const boost::json::object& value, const char* key) {
    const auto& item = member(value, key);
    if (!item.is_int64()) throw std::runtime_error(std::string("non-integer ") + key);
    return item.as_int64();
}

[[nodiscard]] MarketEvent decode(const boost::json::object& document,
                                 const BinancePublicFeedConfig& config,
                                 std::uint64_t sequence,
                                 std::int64_t& best_bid,
                                 std::int64_t& best_ask,
                                 std::int64_t& bid_quantity,
                                 std::int64_t& ask_quantity) {
    const auto* wrapped = document.if_contains("data");
    const auto& data_object = wrapped == nullptr
                                  ? document
                                  : (wrapped->is_object()
                                         ? wrapped->as_object()
                                         : throw std::runtime_error("invalid event data"));
    const auto& event_name = member(data_object, "e");
    if (!event_name.is_string()) throw std::runtime_error("invalid event name");
    const auto receive = now_ns();
    MarketEvent event{
        .sequence = sequence,
        .exchange_time_ns = int_member(data_object, "E") * 1'000'000,
        .receive_time_ns = receive,
        .best_bid_units = best_bid,
        .best_ask_units = best_ask,
        .bid_quantity_units = bid_quantity,
        .ask_quantity_units = ask_quantity,
        .trade_price_units = std::nullopt,
        .trade_quantity_units = 0,
        .aggressor_side = std::nullopt,
    };
    if (std::string_view(event_name.as_string().c_str()) == "bookTicker") {
        best_bid = parse_scaled(string_member(data_object, "b"), config.price_scale);
        best_ask = parse_scaled(string_member(data_object, "a"), config.price_scale);
        bid_quantity = parse_scaled(string_member(data_object, "B"), config.quantity_scale);
        ask_quantity = parse_scaled(string_member(data_object, "A"), config.quantity_scale);
        event.best_bid_units = best_bid;
        event.best_ask_units = best_ask;
        event.bid_quantity_units = bid_quantity;
        event.ask_quantity_units = ask_quantity;
    } else if (std::string_view(event_name.as_string().c_str()) == "aggTrade") {
        event.trade_price_units = parse_scaled(string_member(data_object, "p"), config.price_scale);
        event.trade_quantity_units = parse_scaled(string_member(data_object, "q"), config.quantity_scale);
        const auto* maker = data_object.if_contains("m");
        event.aggressor_side = maker != nullptr && maker->is_bool() && maker->as_bool()
                                   ? std::optional<Side>{Side::sell}
                                   : std::optional<Side>{Side::buy};
    } else {
        throw std::runtime_error("unsupported Binance event");
    }
    if (event.exchange_time_ns > event.receive_time_ns) event.exchange_time_ns = event.receive_time_ns;
    return event;
}

}  // namespace

void BinancePublicFeed::run(EventHandler on_event, ErrorHandler on_error) {
    stop_requested_.store(false);
    std::uint64_t sequence = 0;
    std::int64_t best_bid = 0;
    std::int64_t best_ask = 0;
    std::int64_t bid_quantity = 0;
    std::int64_t ask_quantity = 0;
    std::chrono::seconds retry_delay{1};

    while (!stop_requested_.load()) {
        try {
            asio::io_context io;
            asio::ssl::context ssl_context(asio::ssl::context::tls_client);
            ssl_context.set_default_verify_paths();
            ssl_context.set_verify_mode(asio::ssl::verify_peer);

            tcp::resolver resolver(io);
            const auto proxy = proxy_from_environment();
            const auto results = proxy.has_value()
                                     ? resolver.resolve(proxy->host, proxy->port)
                                     : resolver.resolve(config_.host, config_.port);
            SslWebSocket ws(io, ssl_context);
            if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), config_.host.c_str())) {
                throw std::runtime_error("failed to set Binance TLS server name");
            }
            beast::get_lowest_layer(ws).connect(results);
            if (proxy.has_value()) establish_proxy_tunnel(beast::get_lowest_layer(ws), config_);
            ws.next_layer().handshake(asio::ssl::stream_base::client);
            ws.handshake(config_.host, "/ws/" + config_.symbol + "@bookTicker");
            ws.read_message_max(1U << 20U);

            retry_delay = std::chrono::seconds{1};
            beast::flat_buffer buffer;
            while (!stop_requested_.load()) {
                buffer.clear();
                beast::error_code read_error;
                beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(1));
                ws.read(buffer, read_error);
                if (read_error == asio::error::operation_aborted ||
                    read_error == beast::error::timeout) {
                    continue;
                }
                if (read_error) throw beast::system_error(read_error);
                const auto payload = beast::buffers_to_string(buffer.data());
                boost::json::error_code parse_error;
                const auto document = boost::json::parse(payload, parse_error);
                if (parse_error || !document.is_object()) {
                    throw std::runtime_error("invalid Binance JSON message");
                }
                auto event = decode(document.as_object(), config_, ++sequence, best_bid, best_ask,
                                    bid_quantity, ask_quantity);
                if (event.trade_price_units.has_value() ||
                    (event.best_bid_units > 0 && event.best_ask_units >= event.best_bid_units)) {
                    on_event(event);
                }
            }
            beast::error_code close_error;
            ws.close(websocket::close_code::normal, close_error);
        } catch (const std::exception& error) {
            if (stop_requested_.load()) break;
            if (on_error) on_error(error.what());
            std::this_thread::sleep_for(retry_delay);
            retry_delay = std::min(retry_delay * 2, std::chrono::seconds{30});
        }
    }
}

}  // namespace astra
