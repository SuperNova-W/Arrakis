#include "arrakis/bar_aggregator/aggregation.hpp"

#include <algorithm>
#include <stdexcept>

namespace arrakis::bar_aggregator {
namespace {
std::chrono::sys_time<std::chrono::milliseconds> timestamp(std::uint64_t value) {
    return std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{value}};
}
std::string bar_key(std::string_view symbol, std::uint64_t start) {
    return std::string(symbol) + ":" + std::to_string(start);
}
std::uint64_t interval_ms(std::string_view interval) {
    return interval == "5m" ? 300000U : 60000U;
}
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
    const auto end = state.bar_start_ms + interval_ms(state.interval);
    const auto bar_id = state.symbol + ":" + state.interval + ":" + std::to_string(state.bar_start_ms);
    return MarketBar{bar_id, state.symbol, state.interval,
        timestamp(state.bar_start_ms), timestamp(end), state.open, state.high, state.low, state.close,
        state.volume, state.trade_count, timestamp(state.first_trade_time_ms), timestamp(state.last_trade_time_ms), true};
}

void BarAggregator::addCompletedMinuteToFiveMinute(const MarketBar& bar) {
    const auto five_start = (static_cast<std::uint64_t>(bar.bar_start.time_since_epoch().count()) / 300000U) * 300000U;
    const auto key = bar_key(bar.symbol, five_start);
    auto [it, inserted] = active_five_minute_bars_.try_emplace(key, BarState{bar.symbol, "5m", five_start,
        bar.open, bar.high, bar.low, bar.close, bar.volume, bar.trade_count,
        static_cast<std::uint64_t>(bar.first_trade_time.time_since_epoch().count()),
        static_cast<std::uint64_t>(bar.last_trade_time.time_since_epoch().count()), false});
    if (!inserted) {
        auto& state = it->second;
        state.high = std::max(state.high, bar.high);
        state.low = std::min(state.low, bar.low);
        state.volume += bar.volume;
        state.trade_count += bar.trade_count;
        if (static_cast<std::uint64_t>(bar.first_trade_time.time_since_epoch().count()) < state.first_trade_time_ms) {
            state.first_trade_time_ms = static_cast<std::uint64_t>(bar.first_trade_time.time_since_epoch().count());
            state.open = bar.open;
        }
        if (static_cast<std::uint64_t>(bar.last_trade_time.time_since_epoch().count()) > state.last_trade_time_ms) {
            state.last_trade_time_ms = static_cast<std::uint64_t>(bar.last_trade_time.time_since_epoch().count());
            state.close = bar.close;
        }
    }
}

std::vector<MarketBar> BarAggregator::finalizeFiveMinuteBars(std::uint64_t watermark_timestamp_ms) {
    std::vector<MarketBar> result;
    for (auto it = active_five_minute_bars_.begin(); it != active_five_minute_bars_.end();) {
        const auto end = it->second.bar_start_ms + 300000U;
        if (end <= watermark_timestamp_ms) {
            result.push_back(finalizeBar(it->second));
            it = active_five_minute_bars_.erase(it);
        } else {
            ++it;
        }
    }
    std::sort(result.begin(), result.end(), [](const MarketBar& left, const MarketBar& right) { return left.event_id < right.event_id; });
    return result;
}

void BarAggregator::prune_deduplication_cache(std::uint64_t current) {
    const auto horizon = static_cast<std::uint64_t>(deduplication_window_minutes_) * 60U * 1000U;
    dedup_cache_.erase(std::remove_if(dedup_cache_.begin(), dedup_cache_.end(), [&](const DedupEntry& item) {
        return current > item.timestamp_ms && current - item.timestamp_ms > horizon;
    }), dedup_cache_.end());
}

std::vector<MarketBar> BarAggregator::onTrade(const market::NormalizedTrade& trade) {
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
    if (finalized_bars_.contains(key) || (symbol_watermarks_[trade.symbol] >= end)) {
        ++late_count_;
        late_trades_.push_back({trade, finalized_bars_.contains(key) ? "bar already finalized" : "lateness exceeded configured threshold"});
        return {};
    }
    auto [it, inserted] = active_bars_.try_emplace(key, BarState{trade.symbol, "1m", start, trade.price, trade.price, trade.price,
        trade.price, trade.volume, 1U, trade.source_timestamp_unix_ms, trade.source_timestamp_unix_ms, false});
    if (!inserted) {
        auto& state = it->second;
        state.high = std::max(state.high, trade.price); state.low = std::min(state.low, trade.price);
        state.volume += trade.volume; ++state.trade_count;
        if (trade.source_timestamp_unix_ms < state.first_trade_time_ms) { state.first_trade_time_ms = trade.source_timestamp_unix_ms; state.open = trade.price; }
        if (trade.source_timestamp_unix_ms > state.last_trade_time_ms) { state.last_trade_time_ms = trade.source_timestamp_unix_ms; state.close = trade.price; }
    }
    return {};
}

std::vector<MarketBar> BarAggregator::advanceWatermark(std::uint64_t watermark) {
    watermark_ms_ = std::max(watermark_ms_, watermark);
    for (const auto& [key, state] : active_bars_) symbol_watermarks_[state.symbol] = std::max(symbol_watermarks_[state.symbol], watermark_ms_);
    std::vector<MarketBar> result;
    for (auto it = active_bars_.begin(); it != active_bars_.end();) {
        const auto end = it->second.bar_start_ms + static_cast<std::uint64_t>(bar_interval_seconds_) * 1000U;
        if (end <= watermark_ms_) { const auto bar = finalizeBar(it->second); addCompletedMinuteToFiveMinute(bar); result.push_back(bar); finalized_bars_.insert(it->first); it = active_bars_.erase(it); }
        else ++it;
    }
    std::sort(result.begin(), result.end(), [](const MarketBar& left, const MarketBar& right) { return left.event_id < right.event_id; });
    const auto five_minute = finalizeFiveMinuteBars(watermark_ms_);
    completed_five_minute_bars_.insert(completed_five_minute_bars_.end(), five_minute.begin(), five_minute.end());
    return result;
}

std::vector<MarketBar> BarAggregator::advanceWatermarkForSymbol(std::string_view symbol, std::uint64_t watermark) {
    watermark_ms_ = std::max(watermark_ms_, watermark);
    symbol_watermarks_[std::string(symbol)] = std::max(symbol_watermarks_[std::string(symbol)], watermark);
    std::vector<MarketBar> result;
    for (auto it = active_bars_.begin(); it != active_bars_.end();) {
        if (it->second.symbol != symbol) { ++it; continue; }
        const auto end = it->second.bar_start_ms + static_cast<std::uint64_t>(bar_interval_seconds_) * 1000U;
        if (end <= watermark) { const auto bar = finalizeBar(it->second); addCompletedMinuteToFiveMinute(bar); result.push_back(bar); finalized_bars_.insert(it->first); it = active_bars_.erase(it); }
        else ++it;
    }
    const auto five_minute = finalizeFiveMinuteBars(watermark);
    completed_five_minute_bars_.insert(completed_five_minute_bars_.end(), five_minute.begin(), five_minute.end());
    return result;
}

std::vector<MarketBar> BarAggregator::drainCompletedFiveMinuteBars() {
    auto result = std::move(completed_five_minute_bars_);
    completed_five_minute_bars_.clear();
    return result;
}

std::vector<LateTrade> BarAggregator::drainLateTrades() { auto result = std::move(late_trades_); late_trades_.clear(); return result; }
}  // namespace arrakis::bar_aggregator
