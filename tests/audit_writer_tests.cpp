#include "astra/audit_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("audit writer flushes queued JSON lines on shutdown") {
    const auto path = std::filesystem::temp_directory_path() /
                      ("astra-audit-" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
    {
        astra::AuditWriter writer(path);
        writer.append(R"({"event":"decision","sequence":1})");
        writer.append(R"({"event":"order","sequence":2})");
        CHECK(writer.healthy());
    }
    std::ifstream input(path);
    std::string first;
    std::string second;
    std::getline(input, first);
    std::getline(input, second);
    CHECK(first == R"({"event":"decision","sequence":1})");
    CHECK(second == R"({"event":"order","sequence":2})");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}
