#include "arrakis/streaming/serialization.hpp"

#include "market_bar.pb.h"
#include "trade_event.pb.h"

#include <chrono>
#include <stdexcept>

namespace arrakis::streaming {
namespace {
template <typename Message>
std::vector<std::byte> encode(const Message& message) {
    std::string bytes;
    if (!message.SerializeToString(&bytes)) throw std::runtime_error("protobuf serialization failed");
    std::vector<std::byte> result(bytes.size());
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}
}

std::vector<std::byte> serialize_trade(const market::NormalizedTrade& trade) {
    ::market::events::v1::TradeEvent message;
    auto* metadata = message.mutable_metadata();
    metadata->set_event_id(trade.event_id);
    metadata->set_event_time_unix_ms(static_cast<std::int64_t>(trade.source_timestamp_unix_ms));
    metadata->set_producer("finnhub-ingestion-v1");
    metadata->set_schema_version("trade-event-v1");
    message.set_symbol(trade.symbol); message.set_price(trade.price); message.set_volume(trade.volume);
    message.set_source_timestamp_unix_ms(static_cast<std::int64_t>(trade.source_timestamp_unix_ms));
    message.set_received_timestamp_unix_ms(static_cast<std::int64_t>(trade.received_timestamp_unix_ms));
    message.set_source(trade.source);
    for (const auto& condition : trade.conditions) message.add_trade_conditions(condition);
    return encode(message);
}

market::NormalizedTrade deserialize_trade(std::span<const std::byte> bytes) {
    ::market::events::v1::TradeEvent message;
    if (!message.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) throw std::runtime_error("invalid TradeEvent protobuf");
    if (!message.has_metadata() || message.metadata().event_id().empty() || message.symbol().empty()) throw std::runtime_error("TradeEvent missing identity");
    market::NormalizedTrade result;
    result.event_id = message.metadata().event_id(); result.source = message.source(); result.symbol = message.symbol();
    result.price = message.price(); result.volume = message.volume();
    result.source_timestamp_unix_ms = static_cast<std::uint64_t>(message.source_timestamp_unix_ms());
    result.received_timestamp_unix_ms = static_cast<std::uint64_t>(message.received_timestamp_unix_ms());
    result.conditions.assign(message.trade_conditions().begin(), message.trade_conditions().end());
    return result;
}

std::vector<std::byte> serialize_bar(const market::MarketBar& bar) {
    ::market::events::v1::MarketBar message;
    auto* metadata = message.mutable_metadata(); metadata->set_event_id(bar.event_id); metadata->set_event_time_unix_ms(bar.bar_end.time_since_epoch().count());
    metadata->set_producer("bar-aggregator-v1"); metadata->set_schema_version("market-bar-v1");
    message.set_symbol(bar.symbol); message.set_interval(bar.interval); message.set_bar_start_unix_ms(bar.bar_start.time_since_epoch().count());
    message.set_bar_end_unix_ms(bar.bar_end.time_since_epoch().count()); message.set_open(bar.open); message.set_high(bar.high); message.set_low(bar.low);
    message.set_close(bar.close); message.set_volume(bar.volume); message.set_trade_count(bar.trade_count);
    message.set_first_trade_timestamp_unix_ms(bar.first_trade_time.time_since_epoch().count()); message.set_last_trade_timestamp_unix_ms(bar.last_trade_time.time_since_epoch().count());
    message.set_finalized(bar.finalized);
    return encode(message);
}
}
