#pragma once

#include "arrakis/bar_aggregator/aggregation.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace arrakis::feature_engine {

using Timestamp = std::chrono::sys_time<std::chrono::milliseconds>;

struct FeatureConfig {
    std::vector<std::string> sector_symbols;
    std::vector<std::string> context_symbols;
    std::size_t maximum_history_bars{120};
    std::size_t deduplication_window_minutes{120};
    std::string feature_version{"sector-features-v1"};
    std::string feature_schema_hash;
};

struct FeatureSchema {
    std::string version;
    std::string hash;
    std::vector<std::string> names;
};

struct FeatureVector {
    std::string target_symbol;
    Timestamp event_time;
    std::string source_bar_event_id;
    std::string feature_version;
    std::string schema_hash;
    std::vector<std::string> names;
    std::vector<double> values;
    std::vector<std::string> source_symbols;
    bool complete_context{true};
};

class SymbolHistory {
public:
    explicit SymbolHistory(std::size_t maximum_bars);
    void add(const bar_aggregator::MarketBar& bar);
    [[nodiscard]] const bar_aggregator::MarketBar* get(Timestamp event_time) const;
    [[nodiscard]] std::vector<const bar_aggregator::MarketBar*> recent(Timestamp event_time, std::size_t count) const;
    [[nodiscard]] std::size_t size() const noexcept { return bars_.size(); }

private:
    std::size_t maximum_bars_;
    std::map<std::int64_t, bar_aggregator::MarketBar> bars_;
};

struct AlignmentBucket {
    Timestamp event_time;
    std::map<std::string, bar_aggregator::MarketBar> bars_by_symbol;
    std::chrono::steady_clock::time_point first_arrival;
    std::vector<std::string> missing_symbols;
};

class TimestampAlignmentBuffer {
public:
    TimestampAlignmentBuffer(std::vector<std::string> required_symbols, std::chrono::seconds timeout);
    [[nodiscard]] bool add(const bar_aggregator::MarketBar& bar);
    [[nodiscard]] std::vector<std::string> missing(Timestamp event_time) const;
    [[nodiscard]] bool complete(Timestamp event_time) const;
    [[nodiscard]] std::optional<AlignmentBucket> take(Timestamp event_time);
    [[nodiscard]] std::vector<AlignmentBucket> expire(std::chrono::steady_clock::time_point now);
    [[nodiscard]] std::size_t size() const noexcept { return buckets_.size(); }

private:
    std::vector<std::string> required_symbols_;
    std::chrono::seconds timeout_;
    std::map<std::int64_t, AlignmentBucket> buckets_;
};

class SectorFeatureEngine {
public:
    explicit SectorFeatureEngine(FeatureConfig config);
    [[nodiscard]] const FeatureSchema& schema() const noexcept { return schema_; }
    [[nodiscard]] std::vector<FeatureVector> add_aligned(
        Timestamp event_time, const std::map<std::string, bar_aggregator::MarketBar>& bars);
    [[nodiscard]] std::size_t history_size(std::string_view symbol) const;

private:
    [[nodiscard]] std::optional<FeatureVector> calculate(std::string_view target, Timestamp event_time) const;
    [[nodiscard]] const SymbolHistory* history(std::string_view symbol) const;
    [[nodiscard]] std::optional<double> log_return(const SymbolHistory& history, Timestamp time, std::size_t window) const;
    [[nodiscard]] std::optional<double> rolling_mean_return(const SymbolHistory& history, Timestamp time, std::size_t window) const;
    [[nodiscard]] std::optional<double> volatility(const SymbolHistory& history, Timestamp time, std::size_t window) const;
    [[nodiscard]] std::optional<double> relative_volume(const SymbolHistory& history, Timestamp time, std::size_t window) const;
    [[nodiscard]] std::optional<double> sma_distance(const SymbolHistory& history, Timestamp time, std::size_t window) const;
    [[nodiscard]] std::optional<double> range(const SymbolHistory& history, Timestamp time) const;
    [[nodiscard]] std::optional<double> rolling_range(const SymbolHistory& history, Timestamp time, std::size_t window) const;
    [[nodiscard]] std::optional<double> correlation(const SymbolHistory& left, const SymbolHistory& right, Timestamp time, std::size_t window) const;
    [[nodiscard]] std::optional<double> beta(const SymbolHistory& left, const SymbolHistory& right, Timestamp time, std::size_t window) const;

    FeatureConfig config_;
    FeatureSchema schema_;
    std::map<std::string, SymbolHistory> histories_;
};

}  // namespace arrakis::feature_engine
