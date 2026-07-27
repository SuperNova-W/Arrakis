#include "feature_engine/feature_engine.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace {
arrakis::bar_aggregator::MarketBar make_bar(const std::string& symbol, int index) {
    const auto end = static_cast<std::int64_t>(index + 1) * 300000;
    const double close = 100.0 + static_cast<double>(index) * 0.1 + static_cast<double>(symbol.size());
    return {symbol + ":5m:" + std::to_string(end), symbol, "5m",
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{end - 300000}},
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{end}},
        close, close + 1.0, close - 1.0, close, 1000.0 + index, 10,
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{end - 300000}},
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{end}}, true};
}
}

TEST(FeatureEngineTest, EmitsOnlyAfterWarmupWithDeterministicSchema) {
    arrakis::feature_engine::FeatureConfig config;
    config.sector_symbols = {"XLC", "XLY", "XLP", "XLE", "XLF", "XLV", "XLI", "XLB", "XLRE", "XLK", "XLU"};
    config.context_symbols = {"SPY", "QQQ", "IWM", "TLT", "HYG", "GLD", "USO"};
    config.maximum_history_bars = 120;
    arrakis::feature_engine::SectorFeatureEngine engine(config);
    std::size_t emitted = 0;
    for (int index = 0; index < 30; ++index) {
        std::map<std::string, arrakis::bar_aggregator::MarketBar> bars;
        for (const auto& symbol : config.sector_symbols) bars.emplace(symbol, make_bar(symbol, index));
        for (const auto& symbol : config.context_symbols) bars.emplace(symbol, make_bar(symbol, index));
        const auto time = std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{static_cast<std::int64_t>(index + 1) * 300000}};
        const auto features = engine.add_aligned(time, bars);
        emitted += features.size();
        for (const auto& feature : features) {
            EXPECT_EQ(feature.names.size(), feature.values.size());
            EXPECT_EQ(feature.names.size(), engine.schema().names.size());
            EXPECT_EQ(feature.schema_hash, engine.schema().hash);
            EXPECT_TRUE(std::all_of(feature.values.begin(), feature.values.end(), [](double value) { return std::isfinite(value); }));
        }
    }
    EXPECT_GT(emitted, 0U);
    EXPECT_EQ(engine.history_size("XLC"), 30U);
}

TEST(AlignmentBufferTest, CompletesOutOfOrderAndExpiresMissingSymbols) {
    using namespace std::chrono_literals;
    arrakis::feature_engine::TimestampAlignmentBuffer buffer({"XLC", "SPY"}, 1s);
    const auto first = make_bar("SPY", 1);
    const auto second = make_bar("XLC", 1);
    EXPECT_TRUE(buffer.add(first));
    EXPECT_FALSE(buffer.complete(first.bar_end));
    EXPECT_FALSE(buffer.add(second));
    EXPECT_TRUE(buffer.complete(first.bar_end));
    ASSERT_TRUE(buffer.take(first.bar_end).has_value());
    EXPECT_EQ(buffer.size(), 0U);
}
