#include "astra/audit_writer.hpp"

#include <utility>

namespace astra {

AuditWriter::AuditWriter(std::filesystem::path path, std::size_t capacity)
    : path_(std::move(path)), capacity_(capacity), thread_([this] { run(); }) {}

AuditWriter::~AuditWriter() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void AuditWriter::append(std::string json_line) {
    {
        const std::lock_guard lock(mutex_);
        if (stopping_ || capacity_ == 0U || queue_.size() >= capacity_) {
            ++dropped_events_;
            return;
        }
        queue_.push_back(std::move(json_line));
    }
    condition_.notify_one();
}

std::uint64_t AuditWriter::dropped_events() const {
    const std::lock_guard lock(mutex_);
    return dropped_events_;
}

bool AuditWriter::healthy() const {
    const std::lock_guard lock(mutex_);
    return healthy_;
}

void AuditWriter::run() {
    std::error_code error;
    if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path(), error);
    std::ofstream output(path_, std::ios::app);
    if (error || !output) {
        const std::lock_guard lock(mutex_);
        healthy_ = false;
        return;
    }

    for (;;) {
        std::deque<std::string> batch;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            batch.swap(queue_);
            if (stopping_ && batch.empty()) break;
        }
        for (const auto& line : batch) output << line << '\n';
        output.flush();
        if (!output) {
            const std::lock_guard lock(mutex_);
            healthy_ = false;
        }
    }
}

}  // namespace astra
