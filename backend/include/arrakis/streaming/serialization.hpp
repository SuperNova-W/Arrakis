#pragma once

#include "arrakis/market/normalization.hpp"

#include <span>
#include <vector>

namespace arrakis::streaming {

[[nodiscard]] std::vector<std::byte> serialize_trade(const market::NormalizedTrade& trade);
[[nodiscard]] market::NormalizedTrade deserialize_trade(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> serialize_bar(const market::MarketBar& bar);

}  // namespace arrakis::streaming
