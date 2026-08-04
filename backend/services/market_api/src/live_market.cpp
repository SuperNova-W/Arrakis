#include "arrakis/market_api/live_market.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace arrakis::market_api {
namespace {

std::vector<LiveEtf> load_universe(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open ETF universe: " + path);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    boost::system::error_code error;
    const auto parsed = boost::json::parse(text, error);
    if (error || !parsed.is_object()) throw std::runtime_error("invalid ETF universe JSON: " + path);

    std::vector<LiveEtf> result;
    for (const auto& [key, category] :
         std::array<std::pair<std::string_view, std::string_view>, 2>{
             std::pair{"sector_etfs", "sector"}, std::pair{"context_etfs", "context"}}) {
        const auto* entries = parsed.as_object().if_contains(key);
        if (entries == nullptr || !entries->is_array()) {
            throw std::runtime_error("ETF universe is missing " + std::string(key));
        }
        for (const auto& entry : entries->as_array()) {
            if (!entry.is_object()) throw std::runtime_error("ETF universe entry must be an object");
            const auto* symbol = entry.as_object().if_contains("symbol");
            const auto* name = entry.as_object().if_contains("name");
            if (symbol == nullptr || name == nullptr || !symbol->is_string() || !name->is_string()) {
                throw std::runtime_error("ETF universe entry must include string symbol and name");
            }
            result.push_back(
                {std::string(symbol->as_string()), std::string(name->as_string()), std::string(category), true});
        }
    }
    return result;
}

bar_aggregator::MarketBar new_bar(
    const market::NormalizedTrade& trade, std::string_view interval, std::uint64_t interval_ms) {
    const auto start_ms = (trade.source_timestamp_unix_ms / interval_ms) * interval_ms;
    const auto timestamp = [](std::uint64_t value) {
        return std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{value}};
    };
    return {
        trade.symbol + ":" + std::string(interval) + ":" + std::to_string(start_ms),
        trade.symbol,
        std::string(interval),
        timestamp(start_ms),
        timestamp(start_ms + interval_ms),
        trade.price,
        trade.price,
        trade.price,
        trade.price,
        trade.volume,
        1U,
        timestamp(trade.source_timestamp_unix_ms),
        timestamp(trade.source_timestamp_unix_ms),
        false,
    };
}

}  // namespace

LiveMarketStore::LiveMarketStore(const std::string& universe_path, std::size_t history_limit)
    : history_limit_(history_limit), etfs_(load_universe(universe_path)) {
    if (history_limit_ == 0) throw std::invalid_argument("live market history limit must be positive");
    for (const auto& etf : etfs_) supported_.insert(etf.symbol);
}

void LiveMarketStore::update_interval(
    SymbolState& state, const market::NormalizedTrade& trade,
    std::string_view interval, std::uint64_t interval_ms) {
    auto& current = interval == "5m" ? state.five_minute : state.one_minute;
    auto& history = interval == "5m" ? state.five_minute_history : state.one_minute_history;
    const auto trade_start = (trade.source_timestamp_unix_ms / interval_ms) * interval_ms;
    if (!current || static_cast<std::uint64_t>(current->bar_start.time_since_epoch().count()) < trade_start) {
        if (current) {
            current->finalized = true;
            history.push_back(*current);
            while (history.size() > history_limit_) history.pop_front();
        }
        current = new_bar(trade, interval, interval_ms);
        return;
    }
    if (static_cast<std::uint64_t>(current->bar_start.time_since_epoch().count()) != trade_start) {
        return;
    }
    current->high = std::max(current->high, trade.price);
    current->low = std::min(current->low, trade.price);
    current->volume += trade.volume;
    ++current->trade_count;
    const auto trade_time =
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{trade.source_timestamp_unix_ms}};
    if (trade_time < current->first_trade_time) {
        current->first_trade_time = trade_time;
        current->open = trade.price;
    }
    if (trade_time >= current->last_trade_time) {
        current->last_trade_time = trade_time;
        current->close = trade.price;
    }
}

bool LiveMarketStore::apply(const market::NormalizedTrade& trade) {
    if (trade.event_id.empty() || trade.symbol.empty() || trade.price <= 0.0 || trade.volume < 0.0) return false;
    std::lock_guard lock(mutex_);
    if (!supported_.contains(trade.symbol) || recent_event_id_set_.contains(trade.event_id)) return false;
    recent_event_ids_.push_back(trade.event_id);
    recent_event_id_set_.insert(trade.event_id);
    while (recent_event_ids_.size() > 100000) {
        recent_event_id_set_.erase(recent_event_ids_.front());
        recent_event_ids_.pop_front();
    }
    auto& state = states_[trade.symbol];
    update_interval(state, trade, "1m", 60000U);
    update_interval(state, trade, "5m", 300000U);
    ++sequence_;
    last_update_ = LiveUpdate{sequence_, trade, *state.one_minute, *state.five_minute};
    updated_.notify_all();
    return true;
}

std::vector<LiveEtf> LiveMarketStore::etfs() const {
    std::lock_guard lock(mutex_);
    return etfs_;
}

bool LiveMarketStore::supports(std::string_view symbol) const {
    std::lock_guard lock(mutex_);
    return supported_.contains(std::string(symbol));
}

std::optional<bar_aggregator::MarketBar> LiveMarketStore::latest(
    std::string_view symbol, std::string_view interval) const {
    std::lock_guard lock(mutex_);
    const auto found = states_.find(std::string(symbol));
    if (found == states_.end()) return std::nullopt;
    return interval == "5m" ? found->second.five_minute : found->second.one_minute;
}

std::vector<bar_aggregator::MarketBar> LiveMarketStore::bars(
    std::string_view symbol,
    std::string_view interval,
    std::optional<std::chrono::sys_time<std::chrono::milliseconds>> from,
    std::optional<std::chrono::sys_time<std::chrono::milliseconds>> to,
    std::size_t limit) const {
    std::lock_guard lock(mutex_);
    const auto found = states_.find(std::string(symbol));
    if (found == states_.end()) return {};
    const auto& history =
        interval == "5m" ? found->second.five_minute_history : found->second.one_minute_history;
    const auto& current = interval == "5m" ? found->second.five_minute : found->second.one_minute;
    std::vector<bar_aggregator::MarketBar> result;
    result.reserve(std::min(limit, history.size() + (current ? 1U : 0U)));
    const auto include = [&](const bar_aggregator::MarketBar& bar) {
        return (!from || bar.bar_end >= *from) && (!to || bar.bar_start < *to);
    };
    for (auto it = history.rbegin(); it != history.rend() && result.size() < limit; ++it) {
        if (include(*it)) result.push_back(*it);
    }
    if (current && include(*current)) result.insert(result.begin(), *current);
    if (result.size() > limit) result.resize(limit);
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.bar_start < right.bar_start;
    });
    return result;
}

std::optional<LiveUpdate> LiveMarketStore::last_update() const {
    std::lock_guard lock(mutex_);
    return last_update_;
}

std::uint64_t LiveMarketStore::sequence() const {
    std::lock_guard lock(mutex_);
    return sequence_;
}

bool LiveMarketStore::wait_for_update(
    std::uint64_t after, std::chrono::milliseconds timeout, LiveUpdate& update) const {
    std::unique_lock lock(mutex_);
    if (!updated_.wait_for(lock, timeout, [&] { return sequence_ > after; })) return false;
    if (!last_update_) return false;
    update = *last_update_;
    return true;
}

}  // namespace arrakis::market_api
