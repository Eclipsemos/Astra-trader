#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace astra {

class StatusStore final {
public:
    void publish(std::uint64_t sequence, std::string json);
    [[nodiscard]] std::string snapshot() const;
    [[nodiscard]] std::uint64_t sequence() const;

private:
    mutable std::mutex mutex_;
    std::uint64_t sequence_{0};
    std::string json_{R"({"schema":"astra.status.v1","status":"starting"})"};
};

class StatusServer final {
public:
    StatusServer(std::shared_ptr<StatusStore> store, std::uint16_t port);
    ~StatusServer();
    StatusServer(const StatusServer&) = delete;
    StatusServer& operator=(const StatusServer&) = delete;

    void stop() noexcept;

private:
    void run();

    std::shared_ptr<StatusStore> store_;
    std::uint16_t port_;
    std::atomic_bool stopping_{false};
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
    std::thread thread_;
};

}  // namespace astra
