#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace astra {

class AuditWriter final {
public:
    explicit AuditWriter(std::filesystem::path path, std::size_t capacity = 4'096);
    ~AuditWriter();
    AuditWriter(const AuditWriter&) = delete;
    AuditWriter& operator=(const AuditWriter&) = delete;

    void append(std::string json_line);
    [[nodiscard]] std::uint64_t dropped_events() const;
    [[nodiscard]] bool healthy() const;

private:
    void run();

    std::filesystem::path path_;
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::string> queue_;
    std::uint64_t dropped_events_{0};
    bool stopping_{false};
    bool healthy_{true};
    std::thread thread_;
};

}  // namespace astra
