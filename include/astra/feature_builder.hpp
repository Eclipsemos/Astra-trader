#pragma once

#include "astra/model.hpp"
#include "astra/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace astra {

class FeatureBuilder final {
public:
    explicit FeatureBuilder(std::size_t return_lookback_events = 100);

    [[nodiscard]] ModelState update(const MarketEvent& event,
                                    std::int64_t current_position_units);

private:
    std::vector<std::int64_t> midpoints_;
    std::size_t next_index_{0};
    std::size_t size_{0};
};

}  // namespace astra
