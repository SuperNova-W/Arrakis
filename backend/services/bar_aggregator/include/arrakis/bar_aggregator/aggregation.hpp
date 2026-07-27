#pragma once

#include "arrakis/market/normalization.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace arrakis::bar_aggregator {

struct MarketBar {
    std::string event_id;
    std::string symbol;
    std::string interval;
    std::chrono::sys_time<std::chrono::milliseconds> bar_start;
    std::chrono::sys_time<std::chrono::milliseconds> bar_end;
    double open{};
    double high{};
    double low{};
    double close{};
    double volume{};
    std::uint64_t trade_count{};
    std::chrono::sys_time<std::chrono::milliseconds> first_trade_time;
    std::chrono::sys_time<std::chrono::milliseconds> last_trade_time;
    bool finalized{};
};

struct LateTrade {
    market::NormalizedTrade trade;
    std::string reason;
};

class BarAggregator {
public:
    BarAggregator(std::int64_t bar_interval_seconds,
                  std::int64_t allowed_lateness_seconds,
                  std::size_t deduplication_window_minutes,
                  std::chrono::milliseconds clock_tick);

    [[nodiscard]] std::vector<MarketBar> onTrade(const market::NormalizedTrade& trade);
    [[nodiscard]] std::vector<MarketBar> advanceWatermark(std::uint64_t watermark_timestamp_ms);
    [[nodiscard]] std::vector<MarketBar> advanceWatermarkForSymbol(
        std::string_view symbol, std::uint64_t watermark_timestamp_ms);
    [[nodiscard]] std::vector<MarketBar> drainCompletedFiveMinuteBars();
    [[nodiscard]] std::vector<LateTrade> drainLateTrades();
    [[nodiscard]] std::size_t duplicate_count() const noexcept { return duplicate_count_; }
    [[nodiscard]] std::size_t late_count() const noexcept { return late_count_; }

private:
    struct BarState {
        std::string symbol;
        std::string interval;
        std::uint64_t bar_start_ms{};
        double open{};
        double high{};
        double low{};
        double close{};
        double volume{};
        std::uint64_t trade_count{};
        std::uint64_t first_trade_time_ms{};
        std::uint64_t last_trade_time_ms{};
        bool finalized{};
    };
    struct DedupEntry { std::string event_id; std::uint64_t timestamp_ms{}; };

    [[nodiscard]] std::chrono::sys_time<std::chrono::milliseconds> floorMinute(std::uint64_t timestamp_ms) const;
    [[nodiscard]] std::string buildBarId(const std::string& symbol, std::uint64_t bar_start_ms) const;
    [[nodiscard]] MarketBar finalizeBar(const BarState& state) const;
    void addCompletedMinuteToFiveMinute(const MarketBar& bar);
    [[nodiscard]] std::vector<MarketBar> finalizeFiveMinuteBars(std::uint64_t watermark_timestamp_ms);
    void prune_deduplication_cache(std::uint64_t timestamp_ms);

    const std::int64_t bar_interval_seconds_;
    const std::int64_t allowed_lateness_seconds_;
    const std::size_t deduplication_window_minutes_;
    const std::chrono::milliseconds clock_tick_;
    std::unordered_map<std::string, BarState> active_bars_;
    std::unordered_set<std::string> finalized_bars_;
    std::vector<DedupEntry> dedup_cache_;
    std::uint64_t watermark_ms_{};
    std::unordered_map<std::string, std::uint64_t> symbol_watermarks_;
    std::unordered_map<std::string, BarState> active_five_minute_bars_;
    std::vector<MarketBar> completed_five_minute_bars_;
    std::size_t duplicate_count_{};
    std::size_t late_count_{};
    std::vector<LateTrade> late_trades_;
};

}  // namespace arrakis::bar_aggregator
