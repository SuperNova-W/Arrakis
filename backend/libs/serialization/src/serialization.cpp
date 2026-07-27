#include "arrakis/serialization/serialization.hpp"

#include "market_bar.pb.h"
#include "late_trade_event.pb.h"
#include "dead_letter_event.pb.h"
#include "trade_event.pb.h"

#include <chrono>
#include <stdexcept>
#include <cstring>

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
    metadata->set_produced_time_unix_ms(static_cast<std::int64_t>(trade.received_timestamp_unix_ms));
    metadata->set_producer("finnhub-ingestion-v1");
    metadata->set_schema_version("trade-event-v1");
    metadata->set_correlation_id(trade.correlation_id);
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

std::vector<std::byte> serialize_bar(const bar_aggregator::MarketBar& bar) {
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

bar_aggregator::MarketBar deserialize_bar(std::span<const std::byte> bytes) {
    ::market::events::v1::MarketBar message;
    if (!message.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) throw std::runtime_error("invalid MarketBar protobuf");
    if (message.symbol().empty() || !message.has_metadata()) throw std::runtime_error("MarketBar missing symbol or metadata");
    if (message.metadata().schema_version() != "market-bar-v1") throw std::runtime_error("unsupported MarketBar schema version");
    bar_aggregator::MarketBar result;
    result.event_id = message.metadata().event_id(); result.symbol = message.symbol(); result.interval = message.interval();
    result.bar_start = std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{message.bar_start_unix_ms()}};
    result.bar_end = std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{message.bar_end_unix_ms()}};
    result.open = message.open(); result.high = message.high(); result.low = message.low(); result.close = message.close(); result.volume = message.volume(); result.trade_count = message.trade_count();
    result.first_trade_time = std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{message.first_trade_timestamp_unix_ms()}};
    result.last_trade_time = std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{message.last_trade_timestamp_unix_ms()}}; result.finalized = message.finalized(); return result;
}

std::vector<std::byte> serialize_late_trade(const market::NormalizedTrade& trade, std::string_view reason) {
    ::market::events::v1::LateTradeEvent message;
    auto* metadata = message.mutable_metadata(); metadata->set_event_id(trade.event_id); metadata->set_event_time_unix_ms(static_cast<std::int64_t>(trade.source_timestamp_unix_ms)); metadata->set_producer("bar-aggregator-v1"); metadata->set_schema_version("late-trade-v1");
    message.set_symbol(trade.symbol); message.set_trade_event_id(trade.event_id); message.set_source_timestamp_unix_ms(static_cast<std::int64_t>(trade.source_timestamp_unix_ms)); message.set_reason(reason);
    return encode(message);
}

std::vector<std::byte> serialize_dead_letter(std::string_view service, std::span<const std::byte> original, std::string_view code, std::string_view description, std::int64_t received_time_ms) {
    ::market::events::v1::DeadLetterEvent message;
    auto* metadata = message.mutable_metadata(); metadata->set_event_id(std::string(service) + ":dead-letter:" + std::to_string(received_time_ms)); metadata->set_event_time_unix_ms(received_time_ms); metadata->set_producer(service); metadata->set_schema_version("dead-letter-v1");
    message.set_source_service(service); message.set_original_payload(std::string(reinterpret_cast<const char*>(original.data()), original.size())); message.set_error_code(code); message.set_error_description(description); message.set_received_timestamp_unix_ms(received_time_ms);
    return encode(message);
}
}
