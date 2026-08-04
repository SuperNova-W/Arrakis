#pragma once

#include "arrakis/bar_aggregator/aggregation.hpp"
#include "arrakis/market/normalization.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace arrakis::market_api {

struct LiveEtf {
    std::string symbol;
    std::string name;
    std::string category;
    bool active{true};
};

struct LiveUpdate {
    std::uint64_t sequence{};
    market::NormalizedTrade trade;
    bar_aggregator::MarketBar one_minute;
    bar_aggregator::MarketBar five_minute;
};

class LiveMarketStore final {
public:
    explicit LiveMarketStore(const std::string& universe_path, std::size_t history_limit = 2000);

    [[nodiscard]] bool apply(const market::NormalizedTrade& trade);
    [[nodiscard]] std::vector<LiveEtf> etfs() const;
    [[nodiscard]] bool supports(std::string_view symbol) const;
    [[nodiscard]] std::optional<bar_aggregator::MarketBar> latest(
        std::string_view symbol, std::string_view interval) const;
    [[nodiscard]] std::vector<bar_aggregator::MarketBar> bars(
        std::string_view symbol,
        std::string_view interval,
        std::optional<std::chrono::sys_time<std::chrono::milliseconds>> from,
        std::optional<std::chrono::sys_time<std::chrono::milliseconds>> to,
        std::size_t limit) const;
    [[nodiscard]] std::optional<LiveUpdate> last_update() const;
    [[nodiscard]] std::uint64_t sequence() const;
    [[nodiscard]] bool wait_for_update(
        std::uint64_t after, std::chrono::milliseconds timeout, LiveUpdate& update) const;

private:
    struct SymbolState {
        std::optional<bar_aggregator::MarketBar> one_minute;
        std::optional<bar_aggregator::MarketBar> five_minute;
        std::deque<bar_aggregator::MarketBar> one_minute_history;
        std::deque<bar_aggregator::MarketBar> five_minute_history;
    };

    void update_interval(SymbolState& state, const market::NormalizedTrade& trade,
                         std::string_view interval, std::uint64_t interval_ms);

    const std::size_t history_limit_;
    mutable std::mutex mutex_;
    mutable std::condition_variable updated_;
    std::vector<LiveEtf> etfs_;
    std::unordered_set<std::string> supported_;
    std::unordered_map<std::string, SymbolState> states_;
    std::deque<std::string> recent_event_ids_;
    std::unordered_set<std::string> recent_event_id_set_;
    std::optional<LiveUpdate> last_update_;
    std::uint64_t sequence_{};
};

}  // namespace arrakis::market_api
