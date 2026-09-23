#pragma once

#include "astra/model.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace astra {

struct LayaGatewayConfig {
    std::string host{"127.0.0.1"};
    std::string port{"8000"};
    std::string model{"english"};
    std::chrono::milliseconds timeout{100};
};

class LayaGateway final {
public:
    explicit LayaGateway(LayaGatewayConfig config = {});
    ~LayaGateway();
    LayaGateway(const LayaGateway&) = delete;
    LayaGateway& operator=(const LayaGateway&) = delete;
    LayaGateway(LayaGateway&&) noexcept;
    LayaGateway& operator=(LayaGateway&&) noexcept;

    [[nodiscard]] ModelDecision decide(const ModelState& state) noexcept;
    void close() noexcept;

    [[nodiscard]] static std::string request_json(const ModelState& state,
                                                  std::string_view model);
    [[nodiscard]] static ModelDecision parse_response(std::string_view body,
                                                      std::uint64_t state_sequence,
                                                      std::int64_t latency_ns);

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}  // namespace astra
