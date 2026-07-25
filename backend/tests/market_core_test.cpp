#include "arrakis/market/finnhub_message.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void parses_batched_trade_messages() {
    constexpr auto payload = R"json({
        "type":"trade",
        "data":[
            {"s":"IWM","p":126.55,"t":1721577104208,"v":3,"c":["@","I"]},
            {"s":"IWM","p":126.56,"t":1721577104209,"v":1.5}
        ]
    })json";

    const auto frame = arrakis::market::parse_finnhub_frame(payload);
    assert(frame.controls.empty());
    assert(frame.trades.size() == 2);

    const auto& trade = frame.trades.front();
    assert(trade.symbol == "IWM");
    assert(trade.price == 126.55);
    assert(trade.size == 3.0);
    assert(trade.timestamp_ms == 1721577104208);
    assert(trade.conditions.size() == 2);

    const auto normalized = arrakis::market::trade_event_to_json(trade);
    assert(normalized.find("\"event_type\":\"trade\"") != std::string::npos);
    assert(normalized.find("\"source\":\"finnhub\"") != std::string::npos);
    assert(normalized.find("\"symbol\":\"IWM\"") != std::string::npos);
}

void parses_error_messages() {
    constexpr auto payload = R"json({"type":"error","msg":"invalid token"})json";
    const auto frame = arrakis::market::parse_finnhub_frame(payload);
    assert(frame.controls.size() == 1);
    assert(frame.controls.front().kind == arrakis::market::ControlKind::error);
    assert(frame.controls.front().message == "invalid token");
}

void rejects_non_array_frames() {
    bool threw = false;
    try {
        static_cast<void>(arrakis::market::parse_finnhub_frame(R"json([{"type":"ping"}])json"));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

}  // namespace

int main() {
    parses_batched_trade_messages();
    parses_error_messages();
    rejects_non_array_frames();
    std::cout << "All market-core tests passed\n";
}
