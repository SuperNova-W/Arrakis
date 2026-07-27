#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace arrakis::market {

struct TradeEvent {
    std::string symbol;
    double price{};
    double size{};
    std::uint64_t timestamp_ms{};
    std::vector<std::string> conditions;
};

enum class ControlKind {
    ping,
    error,
    other,
};

struct ControlMessage {
    ControlKind kind{ControlKind::other};
    std::string message;
};

struct FinnhubFrame {
    std::vector<TradeEvent> trades;
    std::vector<ControlMessage> controls;
};

[[nodiscard]] FinnhubFrame parse_finnhub_frame(std::string_view payload);
[[nodiscard]] std::string trade_event_to_json(const TradeEvent& event);

}  // namespace arrakis::market
