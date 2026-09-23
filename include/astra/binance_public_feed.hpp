#pragma once

#include "astra/types.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <utility>

namespace astra {

struct BinancePublicFeedConfig {
    std::string host{"fstream.binance.com"};
    std::string port{"443"};
    std::string symbol{"btcusdt"};
    std::uint8_t price_scale{2};
    std::uint8_t quantity_scale{3};
};

class BinancePublicFeed final {
public:
    using EventHandler = std::function<void(const MarketEvent&)>;
    using ErrorHandler = std::function<void(std::string)>;

    explicit BinancePublicFeed(BinancePublicFeedConfig config = {})
        : config_(std::move(config)) {}

    // Blocks until stop() is requested or the connection fails. This endpoint is public only;
    // the class intentionally has no API-key, signature, or order-entry surface.
    void run(EventHandler on_event, ErrorHandler on_error = {});
    void stop() noexcept { stop_requested_.store(true); }

private:
    BinancePublicFeedConfig config_;
    std::atomic_bool stop_requested_{false};
};

}  // namespace astra
