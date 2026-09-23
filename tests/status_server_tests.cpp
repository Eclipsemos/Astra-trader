#include "astra/status_server.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <thread>

TEST_CASE("status server stops while no client is connected") {
    const auto started = std::chrono::steady_clock::now();
    {
        auto store = std::make_shared<astra::StatusStore>();
        astra::StatusServer server(store, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::seconds(1));
}
