#pragma once

#include "arrakis/market/finnhub_message.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace arrakis::market {

struct NormalizedTrade {
    std::string event_id;
    std::string source;
    std::string symbol;
    double price{};
    double volume{};
    std::uint64_t source_timestamp_unix_ms{};
    std::vector<std::string> conditions;
    std::string correlation_id;
    std::uint64_t received_timestamp_unix_ms{};
};

[[nodiscard]] NormalizedTrade normalize_trade(const TradeEvent& trade, std::string_view source, std::size_t sequence = 0);

}  // namespace arrakis::market
