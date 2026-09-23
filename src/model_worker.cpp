#include "astra/model_worker.hpp"

#include <exception>
#include <string>
#include <utility>

namespace astra {

ModelWorker::ModelWorker(DecisionFunction decision_function)
    : decision_function_(std::move(decision_function)), thread_([this] { run(); }) {}

ModelWorker::~ModelWorker() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        pending_.reset();
    }
    condition_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void ModelWorker::submit(ModelState state) {
    {
        const std::lock_guard lock(mutex_);
        if (stopping_) return;
        if (pending_.has_value()) ++coalesced_requests_;
        pending_ = std::move(state);
    }
    condition_.notify_one();
}

std::optional<ModelDecision> ModelWorker::try_take() {
    const std::lock_guard lock(mutex_);
    auto result = std::move(completed_);
    completed_.reset();
    return result;
}

std::uint64_t ModelWorker::coalesced_requests() const {
    const std::lock_guard lock(mutex_);
    return coalesced_requests_;
}

bool ModelWorker::stopped() const {
    const std::lock_guard lock(mutex_);
    return stopping_;
}

void ModelWorker::run() {
    for (;;) {
        ModelState state;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
            if (stopping_) return;
            state = *pending_;
            pending_.reset();
        }

        ModelDecision decision;
        try {
            decision = decision_function_(state);
        } catch (const std::exception& error) {
            decision = {.action = ModelAction::hold,
                        .long_probability_ppm = 0,
                        .short_probability_ppm = 0,
                        .hold_probability_ppm = 1'000'000,
                        .latency_ns = 0,
                        .state_sequence = state.sequence,
                        .model = {},
                        .error = error.what()};
        } catch (...) {
            decision = {.action = ModelAction::hold,
                        .long_probability_ppm = 0,
                        .short_probability_ppm = 0,
                        .hold_probability_ppm = 1'000'000,
                        .latency_ns = 0,
                        .state_sequence = state.sequence,
                        .model = {},
                        .error = "unknown model worker error"};
        }

        const std::lock_guard lock(mutex_);
        completed_ = std::move(decision);
    }
}

}  // namespace astra
