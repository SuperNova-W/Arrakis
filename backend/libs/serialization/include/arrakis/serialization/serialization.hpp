#pragma once

#include "arrakis/market/normalization.hpp"
#include "arrakis/bar_aggregator/aggregation.hpp"

#include <span>
#include <vector>

namespace arrakis::streaming {

[[nodiscard]] std::vector<std::byte> serialize_trade(const market::NormalizedTrade& trade);
[[nodiscard]] market::NormalizedTrade deserialize_trade(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> serialize_bar(const bar_aggregator::MarketBar& bar);
[[nodiscard]] bar_aggregator::MarketBar deserialize_bar(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> serialize_late_trade(const market::NormalizedTrade& trade, std::string_view reason);
[[nodiscard]] std::vector<std::byte> serialize_dead_letter(std::string_view service, std::span<const std::byte> original,
                                                            std::string_view code, std::string_view description,
                                                            std::int64_t received_time_ms);

}  // namespace arrakis::streaming
