#include "astra/laya_gateway.hpp"

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Laya request carries a versioned numeric market state", "[laya]") {
    const astra::ModelState state{
        .sequence = 42,
        .exchange_time_ns = 100,
        .state_age_ns = 7,
        .best_bid_units = 10'000,
        .best_ask_units = 10'001,
        .bid_quantity_units = 25,
        .ask_quantity_units = 20,
        .return_100_events_ppm = 50,
        .flow_imbalance_ppm = 100'000,
        .current_position_units = -1,
    };
    const auto parsed = boost::json::parse(astra::LayaGateway::request_json(state, "english"));
    REQUIRE(parsed.is_object());
    const auto& root = parsed.as_object();
    CHECK(root.at("model").as_string() == "english");
    CHECK(root.at("state").as_object().at("schema").as_string() == "astra.market-state.v1");
    CHECK(root.at("questions").as_object().at("direction").as_object()
              .at("criteria").as_object().size() == 3U);
}

TEST_CASE("Laya response maps typed probabilities to integer ppm", "[laya]") {
    constexpr std::string_view response = R"({
      "model":"laya-test",
      "answers":{"direction":{"type":"choice","choice":"short",
        "probabilities":{"long":0.1,"short":0.7,"hold":0.2},"confidence":0.7}},
      "usage":{"input_tokens":42,"output_tokens":0}
    })";
    const auto decision = astra::LayaGateway::parse_response(response, 99, 1234);
    CHECK(decision.succeeded());
    CHECK(decision.action == astra::ModelAction::short_position);
    CHECK(decision.long_probability_ppm == 100'000U);
    CHECK(decision.short_probability_ppm == 700'000U);
    CHECK(decision.hold_probability_ppm == 200'000U);
    CHECK(decision.state_sequence == 99U);
    CHECK(decision.latency_ns == 1234);
}

TEST_CASE("unavailable Laya fails closed to hold", "[laya]") {
    astra::LayaGateway gateway({.host = "127.0.0.1",
                                .port = "1",
                                .model = "english",
                                .timeout = std::chrono::milliseconds(20)});
    const auto decision = gateway.decide({.sequence = 7});
    CHECK_FALSE(decision.succeeded());
    CHECK(decision.action == astra::ModelAction::hold);
    CHECK(decision.hold_probability_ppm == 1'000'000U);
    CHECK(decision.state_sequence == 7U);
}
