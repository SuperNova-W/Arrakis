#include "arrakis/market/normalization.hpp"
#include "arrakis/market/finnhub_message.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace arrakis::market {
namespace {

std::string normalized_symbol(std::string_view input) {
    std::size_t first = 0;
    while (first < input.size() && std::isspace(static_cast<unsigned char>(input[first])) != 0) ++first;
    std::size_t last = input.size();
    while (last > first && std::isspace(static_cast<unsigned char>(input[last - 1])) != 0) --last;
    std::string result(input.substr(first, last - first));
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return result;
}

std::chrono::sys_time<std::chrono::milliseconds> timestamp(std::uint64_t value) {
    return std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{value}};
}

std::string bar_key(std::string_view symbol, std::uint64_t start) {
    return std::string(symbol) + ":" + std::to_string(start);
}

std::string number(double value) {
    std::ostringstream stream;
    stream << std::setprecision(15) << value;
    return stream.str();
}

}  // namespace

NormalizedTrade normalize_trade(const TradeEvent& trade, std::string_view source) {
    const auto symbol = normalized_symbol(trade.symbol);
    if (symbol.empty()) throw std::runtime_error("trade symbol is empty");
    if (!std::isfinite(trade.price) || trade.price <= 0.0) throw std::runtime_error("trade price must be positive and finite");
    if (!std::isfinite(trade.size) || trade.size < 0.0) throw std::runtime_error("trade volume must be non-negative and finite");
    if (trade.timestamp_ms == 0U) throw std::runtime_error("trade timestamp is invalid");

    NormalizedTrade result;
    result.source = std::string(source);
    result.symbol = symbol;
    result.price = trade.price;
    result.volume = trade.size;
    result.source_timestamp_unix_ms = trade.timestamp_ms;
    result.conditions = trade.conditions;
    // Finnhub does not expose a sequence number in every trade payload. The canonical
    // tuple remains deterministic and is intentionally stable across reconnects.
    result.event_id = result.source + ":" + result.symbol + ":" +
        std::to_string(result.source_timestamp_unix_ms) + ":" +
        number(result.price) + ":" + number(result.volume) + ":0";
    return result;
}

BarAggregator::BarAggregator(std::int64_t interval_seconds, std::int64_t allowed_lateness_seconds,
                             std::size_t deduplication_window_minutes, std::chrono::milliseconds clock_tick)
    : bar_interval_seconds_(interval_seconds), allowed_lateness_seconds_(allowed_lateness_seconds),
      deduplication_window_minutes_(deduplication_window_minutes), clock_tick_(clock_tick) {
    if (bar_interval_seconds_ <= 0 || allowed_lateness_seconds_ < 0 || deduplication_window_minutes_ == 0 || clock_tick_.count() <= 0) {
        throw std::invalid_argument("invalid bar aggregator configuration");
    }
}

std::chrono::sys_time<std::chrono::milliseconds> BarAggregator::floorMinute(std::uint64_t value) const {
    const auto interval_ms = static_cast<std::uint64_t>(bar_interval_seconds_) * 1000U;
    return timestamp((value / interval_ms) * interval_ms);
}

std::string BarAggregator::buildBarId(const std::string& symbol, std::uint64_t start) const {
    return symbol + ":1m:" + std::to_string(start);
}

MarketBar BarAggregator::finalizeBar(const BarState& state) const {
    const auto end = state.bar_start_ms + static_cast<std::uint64_t>(bar_interval_seconds_) * 1000U;
    return MarketBar{buildBarId(state.symbol, state.bar_start_ms), state.symbol, "1m",
        timestamp(state.bar_start_ms), timestamp(end), state.open, state.high, state.low, state.close,
        state.volume, state.trade_count, timestamp(state.first_trade_time_ms), timestamp(state.last_trade_time_ms), true};
}

void BarAggregator::prune_deduplication_cache(std::uint64_t current) {
    const auto horizon = static_cast<std::uint64_t>(deduplication_window_minutes_) * 60U * 1000U;
    dedup_cache_.erase(std::remove_if(dedup_cache_.begin(), dedup_cache_.end(), [&](const DedupEntry& item) {
        return current > item.timestamp_ms && current - item.timestamp_ms > horizon;
    }), dedup_cache_.end());
}

std::vector<MarketBar> BarAggregator::onTrade(const NormalizedTrade& trade) {
    if (trade.event_id.empty() || trade.symbol.empty()) return {};
    prune_deduplication_cache(trade.source_timestamp_unix_ms);
    if (std::any_of(dedup_cache_.begin(), dedup_cache_.end(), [&](const DedupEntry& item) { return item.event_id == trade.event_id; })) {
        ++duplicate_count_;
        return {};
    }
    dedup_cache_.push_back({trade.event_id, trade.source_timestamp_unix_ms});

    const auto start = static_cast<std::uint64_t>(floorMinute(trade.source_timestamp_unix_ms).time_since_epoch().count());
    const auto end = start + static_cast<std::uint64_t>(bar_interval_seconds_) * 1000U;
    const auto key = bar_key(trade.symbol, start);
    if (finalized_bars_.contains(key) || (watermark_ms_ >= end)) {
        ++late_count_;
        return {};
    }

    auto [it, inserted] = active_bars_.try_emplace(key, BarState{trade.symbol, start, trade.price, trade.price, trade.price,
        trade.price, trade.volume, 1U, trade.source_timestamp_unix_ms, trade.source_timestamp_unix_ms, false});
    if (!inserted) {
        auto& state = it->second;
        state.high = std::max(state.high, trade.price);
        state.low = std::min(state.low, trade.price);
        state.volume += trade.volume;
        ++state.trade_count;
        if (trade.source_timestamp_unix_ms < state.first_trade_time_ms) {
            state.first_trade_time_ms = trade.source_timestamp_unix_ms;
            state.open = trade.price;
        }
        if (trade.source_timestamp_unix_ms > state.last_trade_time_ms) {
            state.last_trade_time_ms = trade.source_timestamp_unix_ms;
            state.close = trade.price;
        }
    }
    return {};
}

std::vector<MarketBar> BarAggregator::advanceWatermark(std::uint64_t watermark) {
    watermark_ms_ = std::max(watermark_ms_, watermark);
    std::vector<MarketBar> result;
    for (auto it = active_bars_.begin(); it != active_bars_.end();) {
        const auto end = it->second.bar_start_ms + static_cast<std::uint64_t>(bar_interval_seconds_) * 1000U;
        if (end <= watermark_ms_) {
            result.push_back(finalizeBar(it->second));
            finalized_bars_.insert(it->first);
            it = active_bars_.erase(it);
        } else {
            ++it;
        }
    }
    std::sort(result.begin(), result.end(), [](const MarketBar& left, const MarketBar& right) {
        return left.event_id < right.event_id;
    });
    return result;
}

}  // namespace arrakis::market
