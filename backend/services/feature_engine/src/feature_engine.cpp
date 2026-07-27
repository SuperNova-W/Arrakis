#include "feature_engine/feature_engine.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <sstream>

namespace arrakis::feature_engine {
namespace {
double value(const bar_aggregator::MarketBar& bar) { return bar.close; }
std::int64_t ms(Timestamp time) { return time.time_since_epoch().count(); }
std::string hash_schema(const std::vector<std::string>& names) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto& name : names) { for (const char c : name) { hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ULL; } hash ^= static_cast<std::uint64_t>('|'); hash *= 1099511628211ULL; }
    std::ostringstream output; output << std::hex << std::setw(16) << std::setfill('0') << hash; return output.str();
}
std::vector<std::string> schema_names() {
    return {"return_1","return_3","return_6","return_12","rolling_mean_return_6","rolling_mean_return_12",
        "rolling_volatility_6","rolling_volatility_12","rolling_volatility_24","relative_volume_20",
        "close_to_sma_12","close_to_sma_24","high_low_range_1","rolling_high_low_range_12",
        "SPY_return_1","SPY_return_3","SPY_return_6","QQQ_return_1","QQQ_return_3","QQQ_return_6",
        "IWM_return_1","IWM_return_3","IWM_return_6","TLT_return_1","TLT_return_6","HYG_return_1","HYG_return_6",
        "GLD_return_1","GLD_return_6","USO_return_1","USO_return_6","sector_minus_SPY_return_1",
        "sector_minus_SPY_return_3","sector_minus_SPY_return_6","sector_minus_QQQ_return_1","sector_minus_QQQ_return_6",
        "sector_return_rank_1","sector_return_rank_3","sector_return_rank_6","sector_volatility_rank_12",
        "rolling_correlation_SPY_24","rolling_beta_SPY_24","rolling_correlation_QQQ_24"};
}
bool finite(double number) { return std::isfinite(number); }
std::optional<double> rank(const std::vector<double>& values, double target) {
    if (values.size() < 2) return std::nullopt;
    std::vector<double> sorted = values; std::sort(sorted.begin(), sorted.end());
    const auto first = std::lower_bound(sorted.begin(), sorted.end(), target);
    const auto last = std::upper_bound(sorted.begin(), sorted.end(), target);
    const double average_rank = (static_cast<double>(first - sorted.begin()) + static_cast<double>(last - sorted.begin() - 1)) / 2.0;
    return average_rank / static_cast<double>(sorted.size() - 1);
}
}

SymbolHistory::SymbolHistory(std::size_t maximum_bars) : maximum_bars_(maximum_bars) {}
void SymbolHistory::add(const bar_aggregator::MarketBar& bar) {
    bars_[ms(bar.bar_end)] = bar;
    while (bars_.size() > maximum_bars_) bars_.erase(bars_.begin());
}
const bar_aggregator::MarketBar* SymbolHistory::get(Timestamp event_time) const {
    const auto it = bars_.find(ms(event_time)); return it == bars_.end() ? nullptr : &it->second;
}
std::vector<const bar_aggregator::MarketBar*> SymbolHistory::recent(Timestamp event_time, std::size_t count) const {
    std::vector<const bar_aggregator::MarketBar*> result; auto it = bars_.upper_bound(ms(event_time));
    while (it != bars_.begin() && result.size() < count) { --it; result.push_back(&it->second); }
    return result;
}

TimestampAlignmentBuffer::TimestampAlignmentBuffer(std::vector<std::string> required_symbols, std::chrono::seconds timeout)
    : required_symbols_(std::move(required_symbols)), timeout_(timeout) {}
bool TimestampAlignmentBuffer::add(const bar_aggregator::MarketBar& bar) {
    const auto key = ms(bar.bar_end); auto [it, inserted] = buckets_.try_emplace(key, AlignmentBucket{bar.bar_end, {}, std::chrono::steady_clock::now(), {}});
    it->second.bars_by_symbol[bar.symbol] = bar; return inserted;
}
bool TimestampAlignmentBuffer::complete(Timestamp event_time) const {
    const auto it = buckets_.find(ms(event_time)); if (it == buckets_.end()) return false;
    return std::all_of(required_symbols_.begin(), required_symbols_.end(), [&](const std::string& symbol) { return it->second.bars_by_symbol.contains(symbol); });
}
std::vector<std::string> TimestampAlignmentBuffer::missing(Timestamp event_time) const {
    std::vector<std::string> result; const auto it = buckets_.find(ms(event_time)); if (it == buckets_.end()) return result;
    for (const auto& symbol : required_symbols_) if (!it->second.bars_by_symbol.contains(symbol)) result.push_back(symbol); return result;
}
std::optional<AlignmentBucket> TimestampAlignmentBuffer::take(Timestamp event_time) {
    const auto it = buckets_.find(ms(event_time)); if (it == buckets_.end()) return std::nullopt;
    auto result = std::move(it->second); buckets_.erase(it); return result;
}
std::vector<AlignmentBucket> TimestampAlignmentBuffer::expire(std::chrono::steady_clock::time_point now) {
    std::vector<AlignmentBucket> result;
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        if (now - it->second.first_arrival >= timeout_) { for (const auto& symbol : required_symbols_) if (!it->second.bars_by_symbol.contains(symbol)) it->second.missing_symbols.push_back(symbol); result.push_back(std::move(it->second)); it = buckets_.erase(it); } else ++it;
    }
    return result;
}

SectorFeatureEngine::SectorFeatureEngine(FeatureConfig config) : config_(std::move(config)), schema_{config_.feature_version, {}, schema_names()} {
    schema_.hash = hash_schema(schema_.names); if (!config_.feature_schema_hash.empty() && config_.feature_schema_hash != schema_.hash) throw std::invalid_argument("feature schema hash does not match canonical schema");
    for (const auto& symbol : config_.sector_symbols) histories_.emplace(symbol, SymbolHistory(config_.maximum_history_bars));
    for (const auto& symbol : config_.context_symbols) histories_.emplace(symbol, SymbolHistory(config_.maximum_history_bars));
}
const SymbolHistory* SectorFeatureEngine::history(std::string_view symbol) const { const auto it = histories_.find(std::string(symbol)); return it == histories_.end() ? nullptr : &it->second; }
std::size_t SectorFeatureEngine::history_size(std::string_view symbol) const { const auto* item = history(symbol); return item == nullptr ? 0U : item->size(); }

std::optional<double> SectorFeatureEngine::log_return(const SymbolHistory& history, Timestamp time, std::size_t window) const {
    const auto bars = history.recent(time, window + 1); if (bars.size() <= window || value(*bars[0]) <= 0.0 || value(*bars[window]) <= 0.0) return std::nullopt;
    return std::log(value(*bars[0]) / value(*bars[window]));
}
std::optional<double> SectorFeatureEngine::rolling_mean_return(const SymbolHistory& history, Timestamp time, std::size_t window) const {
    const auto bars = history.recent(time, window + 1); if (bars.size() <= window) return std::nullopt; double total = 0.0;
    for (std::size_t i = 0; i < window; ++i) { if (value(*bars[i]) <= 0.0 || value(*bars[i + 1]) <= 0.0) return std::nullopt; total += std::log(value(*bars[i]) / value(*bars[i + 1])); } return total / static_cast<double>(window);
}
std::optional<double> SectorFeatureEngine::volatility(const SymbolHistory& history, Timestamp time, std::size_t window) const {
    const auto bars = history.recent(time, window + 1); if (bars.size() <= window || window < 2) return std::nullopt; std::vector<double> returns; returns.reserve(window);
    for (std::size_t i = 0; i < window; ++i) { if (value(*bars[i]) <= 0.0 || value(*bars[i + 1]) <= 0.0) return std::nullopt; returns.push_back(std::log(value(*bars[i]) / value(*bars[i + 1]))); }
    const double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / static_cast<double>(window); double sum = 0.0; for (double item : returns) sum += (item - mean) * (item - mean); return std::sqrt(sum / static_cast<double>(window - 1));
}
std::optional<double> SectorFeatureEngine::relative_volume(const SymbolHistory& history, Timestamp time, std::size_t window) const {
    const auto bars = history.recent(time, window + 1); if (bars.size() <= window || bars[0]->volume < 0.0) return std::nullopt; double mean = 0.0; for (std::size_t i = 1; i <= window; ++i) mean += bars[i]->volume; mean /= static_cast<double>(window); return mean > 0.0 ? std::optional<double>{bars[0]->volume / mean} : std::nullopt;
}
std::optional<double> SectorFeatureEngine::sma_distance(const SymbolHistory& history, Timestamp time, std::size_t window) const {
    const auto bars = history.recent(time, window); if (bars.size() < window || value(*bars[0]) <= 0.0) return std::nullopt; double mean = 0.0; for (const auto* bar : bars) mean += value(*bar); mean /= static_cast<double>(window); return mean > 0.0 ? std::optional<double>{value(*bars[0]) / mean - 1.0} : std::nullopt;
}
std::optional<double> SectorFeatureEngine::range(const SymbolHistory& history, Timestamp time) const { const auto* bar = history.get(time); return bar == nullptr || bar->close <= 0.0 ? std::nullopt : std::optional<double>{(bar->high - bar->low) / bar->close}; }
std::optional<double> SectorFeatureEngine::rolling_range(const SymbolHistory& history, Timestamp time, std::size_t window) const { const auto bars = history.recent(time, window); if (bars.size() < window) return std::nullopt; double total = 0.0; for (const auto* bar : bars) { if (bar->close <= 0.0) return std::nullopt; total += (bar->high - bar->low) / bar->close; } return total / static_cast<double>(window); }

std::optional<double> SectorFeatureEngine::correlation(const SymbolHistory& left, const SymbolHistory& right, Timestamp time, std::size_t window) const {
    const auto a = left.recent(time, window + 1); const auto b = right.recent(time, window + 1); if (a.size() <= window || b.size() <= window) return std::nullopt; std::vector<double> x,y; for (std::size_t i=0;i<window;++i) { if (value(*a[i])<=0 || value(*a[i+1])<=0 || value(*b[i])<=0 || value(*b[i+1])<=0) return std::nullopt; x.push_back(std::log(value(*a[i])/value(*a[i+1]))); y.push_back(std::log(value(*b[i])/value(*b[i+1]))); } const double window_size=static_cast<double>(window); const double mx=std::accumulate(x.begin(),x.end(),0.0)/window_size, my=std::accumulate(y.begin(),y.end(),0.0)/window_size; double cov=0,vx=0,vy=0; for(std::size_t i=0;i<window;++i){const double dx=x[i]-mx,dy=y[i]-my;cov+=dx*dy;vx+=dx*dx;vy+=dy*dy;} return vx>1e-14&&vy>1e-14?std::optional<double>{cov/std::sqrt(vx*vy)}:std::nullopt;
}
std::optional<double> SectorFeatureEngine::beta(const SymbolHistory& left, const SymbolHistory& right, Timestamp time, std::size_t window) const {
    const auto a = left.recent(time, window + 1); const auto b = right.recent(time, window + 1); if (a.size() <= window || b.size() <= window) return std::nullopt; std::vector<double> x,y; for(std::size_t i=0;i<window;++i){if(value(*a[i])<=0||value(*a[i+1])<=0||value(*b[i])<=0||value(*b[i+1])<=0)return std::nullopt;x.push_back(std::log(value(*a[i])/value(*a[i+1])));y.push_back(std::log(value(*b[i])/value(*b[i+1])));} const double window_size=static_cast<double>(window); const double mx=std::accumulate(x.begin(),x.end(),0.0)/window_size,my=std::accumulate(y.begin(),y.end(),0.0)/window_size;double cov=0,var=0;for(std::size_t i=0;i<window;++i){cov+=(x[i]-mx)*(y[i]-my);var+=(y[i]-my)*(y[i]-my);}return var>1e-14?std::optional<double>{cov/var}:std::nullopt;
}

std::optional<FeatureVector> SectorFeatureEngine::calculate(std::string_view target, Timestamp time) const {
    const auto* target_history = history(target); const auto* spy = history("SPY"); const auto* qqq = history("QQQ"); if (!target_history || !spy || !qqq) return std::nullopt;
    std::vector<double> values; values.reserve(schema_.names.size()); auto add = [&](std::optional<double> item) { if (!item || !finite(*item)) return false; values.push_back(*item); return true; };
    if (!add(log_return(*target_history,time,1))||!add(log_return(*target_history,time,3))||!add(log_return(*target_history,time,6))||!add(log_return(*target_history,time,12))||!add(rolling_mean_return(*target_history,time,6))||!add(rolling_mean_return(*target_history,time,12))||!add(volatility(*target_history,time,6))||!add(volatility(*target_history,time,12))||!add(volatility(*target_history,time,24))||!add(relative_volume(*target_history,time,20))||!add(sma_distance(*target_history,time,12))||!add(sma_distance(*target_history,time,24))||!add(range(*target_history,time))||!add(rolling_range(*target_history,time,12))) return std::nullopt;
    const auto context = [&](std::string_view symbol, std::size_t window) -> std::optional<double> { const auto* item=history(symbol); return item==nullptr?std::nullopt:log_return(*item,time,window); };
    for (const auto& symbol : {std::string("SPY"),std::string("QQQ"),std::string("IWM")}) for (std::size_t window : {1U,3U,6U}) if(!add(context(symbol,window))) return std::nullopt;
    for (const auto& symbol : {std::string("TLT"),std::string("HYG"),std::string("GLD"),std::string("USO")}) for (std::size_t window : {1U,6U}) if(!add(context(symbol,window))) return std::nullopt;
    const auto sector_return = [&](std::size_t window){return log_return(*target_history,time,window);}; const auto spy_return = [&](std::size_t window){return log_return(*spy,time,window);}; const auto qqq_return = [&](std::size_t window){return log_return(*qqq,time,window);};
    for (std::size_t window : {1U,3U,6U}) { const auto a=sector_return(window),b=spy_return(window); if(!a||!b||!add(*a-*b)) return std::nullopt; } { const auto a=sector_return(1),b=qqq_return(1); if(!a||!b||!add(*a-*b)) return std::nullopt; } { const auto a=sector_return(6),b=qqq_return(6); if(!a||!b||!add(*a-*b)) return std::nullopt; }
    std::vector<double> r1,r3,r6,v12; for(const auto& symbol:config_.sector_symbols){const auto* item=history(symbol);if(!item)return std::nullopt;auto a=log_return(*item,time,1),b=log_return(*item,time,3),c=log_return(*item,time,6),d=volatility(*item,time,12);if(!a||!b||!c||!d)return std::nullopt;r1.push_back(*a);r3.push_back(*b);r6.push_back(*c);v12.push_back(*d);}
    const auto rank_for = [&](const std::vector<double>& ranks){const auto it=std::find(config_.sector_symbols.begin(),config_.sector_symbols.end(),target);return it==config_.sector_symbols.end()?std::nullopt:rank(ranks,ranks[static_cast<std::size_t>(it-config_.sector_symbols.begin())]);}; if(!add(rank_for(r1))||!add(rank_for(r3))||!add(rank_for(r6))||!add(rank_for(v12)))return std::nullopt;
    if(!add(correlation(*target_history,*spy,time,24))||!add(beta(*target_history,*spy,time,24))||!add(correlation(*target_history,*qqq,time,24)))return std::nullopt;
    const auto* target_bar=target_history->get(time); if(!target_bar||values.size()!=schema_.names.size())return std::nullopt;
    FeatureVector result; result.target_symbol=std::string(target);result.event_time=time;result.source_bar_event_id=target_bar->event_id;result.feature_version=schema_.version;result.schema_hash=schema_.hash;result.names=schema_.names;result.values=std::move(values);result.source_symbols=config_.sector_symbols;result.source_symbols.insert(result.source_symbols.end(),config_.context_symbols.begin(),config_.context_symbols.end());return result;
}

std::vector<FeatureVector> SectorFeatureEngine::add_aligned(Timestamp event_time, const std::map<std::string, bar_aggregator::MarketBar>& bars) {
    for (const auto& [symbol,bar] : bars) { const auto it=histories_.find(symbol); if(it!=histories_.end()) it->second.add(bar); }
    std::vector<FeatureVector> result; for(const auto& target:config_.sector_symbols){const auto feature=calculate(target,event_time);if(feature)result.push_back(*feature);} return result;
}
}  // namespace arrakis::feature_engine
