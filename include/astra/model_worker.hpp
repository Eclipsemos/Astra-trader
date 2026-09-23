#pragma once

#include "astra/model.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace astra {

class ModelWorker final {
public:
    using DecisionFunction = std::function<ModelDecision(const ModelState&)>;

    explicit ModelWorker(DecisionFunction decision_function);
    ~ModelWorker();
    ModelWorker(const ModelWorker&) = delete;
    ModelWorker& operator=(const ModelWorker&) = delete;

    void submit(ModelState state);
    [[nodiscard]] std::optional<ModelDecision> try_take();
    [[nodiscard]] std::uint64_t coalesced_requests() const;
    [[nodiscard]] bool stopped() const;

private:
    void run();

    DecisionFunction decision_function_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<ModelState> pending_;
    std::optional<ModelDecision> completed_;
    std::uint64_t coalesced_requests_{0};
    bool stopping_{false};
    std::thread thread_;
};

}  // namespace astra
