#include "arrakis/market/bar_aggregator.hpp"
#include "arrakis/market/finnhub_message.hpp"
#include "arrakis/market/normalization.hpp"

#include <gtest/gtest.h>

namespace {

TEST(FinnhubNormalizationTest, ParsesAndNormalizesTradePayload) {
    constexpr auto payload = R"json({
        "type":"trade",
        "data":[
            {"s":"SPY","p":520.75,"t":1721577104208,"v":3,"c":["@","I"]}
        ]
    })json";

    const auto frame = arrakis::market::parse_finnhub_frame(payload);
    ASSERT_EQ(frame.trades.size(), 1U);

    const auto normalized = arrakis::market::normalize_trade(frame.trades.front(), "finnhub");
    EXPECT_EQ(normalized.symbol, "SPY");
    EXPECT_DOUBLE_EQ(normalized.price, 520.75);
    EXPECT_DOUBLE_EQ(normalized.volume, 3.0);
    EXPECT_EQ(normalized.source_timestamp_unix_ms, 1721577104208ULL);
    EXPECT_EQ(normalized.event_id, "finnhub:SPY:1721577104208:520.75:3:0");
    EXPECT_EQ(normalized.conditions.size(), 2U);
}

TEST(BarAggregatorTest, BuildsOneMinuteBarFromTrades) {
    arrakis::market::BarAggregator aggregator(
        60,
        5,
        10,
        std::chrono::seconds(1)
    );

    const arrakis::market::NormalizedTrade trade1 = {
        "finnhub:SPY:1721577104208:520.75:3:0",
        "finnhub",
        "SPY",
        520.75,
        3.0,
        1721577104208ULL,
        std::vector<std::string>{"@"},
        std::string{}
    };
    const arrakis::market::NormalizedTrade trade2 = {
        "finnhub:SPY:1721577104209:521.10:2:0",
        "finnhub",
        "SPY",
        521.10,
        2.0,
        1721577104209ULL,
        std::vector<std::string>{"I"},
        std::string{}
    };

    auto bars = aggregator.onTrade(trade1);
    EXPECT_TRUE(bars.empty());

    bars = aggregator.onTrade(trade2);
    EXPECT_TRUE(bars.empty());
    bars = aggregator.advanceWatermark(1721577164208ULL);
    ASSERT_EQ(bars.size(), 1U);
    EXPECT_EQ(bars.front().symbol, "SPY");
    EXPECT_EQ(bars.front().interval, "1m");
    EXPECT_DOUBLE_EQ(bars.front().open, 520.75);
    EXPECT_DOUBLE_EQ(bars.front().high, 521.10);
    EXPECT_DOUBLE_EQ(bars.front().low, 520.75);
    EXPECT_DOUBLE_EQ(bars.front().close, 521.10);
    EXPECT_DOUBLE_EQ(bars.front().volume, 5.0);
    EXPECT_EQ(bars.front().trade_count, 2U);
}

TEST(BarAggregatorTest, FinalizesBarAtWatermark) {
    arrakis::market::BarAggregator aggregator(
        60,
        5,
        10,
        std::chrono::seconds(1)
    );

    const arrakis::market::NormalizedTrade trade = {
        "finnhub:SPY:1721577104208:520.75:3:0",
        "finnhub",
        "SPY",
        520.75,
        3.0,
        1721577104208ULL,
        std::vector<std::string>{"@"},
        std::string{}
    };

    EXPECT_TRUE(aggregator.onTrade(trade).empty());

    const auto finalized = aggregator.advanceWatermark(1721577164208ULL);
    ASSERT_EQ(finalized.size(), 1U);
    EXPECT_TRUE(finalized.front().finalized);
    EXPECT_EQ(finalized.front().symbol, "SPY");
}

}  // namespace
