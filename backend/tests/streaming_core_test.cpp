#include "arrakis/bar_aggregator/aggregation.hpp"
#include "arrakis/market/finnhub_message.hpp"
#include "arrakis/market/normalization.hpp"
#include "arrakis/serialization/serialization.hpp"

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
    EXPECT_EQ(normalized.event_id, "finnhub:sha256:5930a0a0ae5747c4e2abf2d06c0b864433e5dded7424190d9f71db8624eddbcf");
    EXPECT_EQ(normalized.conditions.size(), 2U);
}

TEST(BarAggregatorTest, BuildsOneMinuteBarFromTrades) {
    arrakis::bar_aggregator::BarAggregator aggregator(
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
    arrakis::bar_aggregator::BarAggregator aggregator(
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

TEST(BarAggregatorTest, DerivesFiveMinuteBarFromCompletedMinutes) {
    arrakis::bar_aggregator::BarAggregator aggregator(60, 5, 10, std::chrono::seconds(1));
    for (std::uint64_t minute = 0; minute < 5; ++minute) {
        const auto timestamp = 1ULL + minute * 60000ULL;
        const arrakis::market::NormalizedTrade trade{
            "finnhub:SPY:" + std::to_string(timestamp), "finnhub", "SPY",
            100.0 + static_cast<double>(minute), 2.0 + static_cast<double>(minute), timestamp,
            {}, {}, 0ULL
        };
        EXPECT_TRUE(aggregator.onTrade(trade).empty());
    }

    const auto one_minute = aggregator.advanceWatermark(360000ULL);
    ASSERT_EQ(one_minute.size(), 5U);
    const auto five_minute = aggregator.drainCompletedFiveMinuteBars();
    ASSERT_EQ(five_minute.size(), 1U);
    EXPECT_EQ(five_minute.front().interval, "5m");
    EXPECT_EQ(five_minute.front().bar_start.time_since_epoch().count(), 0);
    EXPECT_DOUBLE_EQ(five_minute.front().open, 100.0);
    EXPECT_DOUBLE_EQ(five_minute.front().high, 104.0);
    EXPECT_DOUBLE_EQ(five_minute.front().low, 100.0);
    EXPECT_DOUBLE_EQ(five_minute.front().close, 104.0);
    EXPECT_DOUBLE_EQ(five_minute.front().volume, 20.0);
    EXPECT_EQ(five_minute.front().trade_count, 5U);
}

TEST(BarAggregatorTest, RejectsFinalizedTradeAsLateEvent) {
    arrakis::bar_aggregator::BarAggregator aggregator(60, 5, 10, std::chrono::seconds(1));
    const arrakis::market::NormalizedTrade trade{
        "finnhub:SPY:60000:100:1:0", "finnhub", "SPY", 100.0, 1.0, 60000ULL,
        {}, {}, 0ULL
    };
    const auto late_trade = arrakis::market::NormalizedTrade{
        "finnhub:SPY:60001:101:1:0", "finnhub", "SPY", 101.0, 1.0, 60000ULL,
        {}, {}, 0ULL
    };
    EXPECT_TRUE(aggregator.onTrade(late_trade).empty());
    ASSERT_EQ(aggregator.advanceWatermark(120000ULL).size(), 1U);
    EXPECT_TRUE(aggregator.onTrade(trade).empty());
    const auto late = aggregator.drainLateTrades();
    ASSERT_EQ(late.size(), 1U);
    EXPECT_EQ(late.front().reason, "bar already finalized");
    EXPECT_EQ(aggregator.late_count(), 1U);
}

TEST(ProtobufSerializationTest, TradeRoundTripPreservesIdentityAndFields) {
    const arrakis::market::NormalizedTrade input{
        "finnhub:SPY:1721577104208:520.75:3:0", "finnhub", "SPY", 520.75, 3.0,
        1721577104208ULL, {"@", "I"}, "corr-1", 1721577104209ULL
    };
    const auto bytes = arrakis::streaming::serialize_trade(input);
    const auto output = arrakis::streaming::deserialize_trade(bytes);
    EXPECT_EQ(output.event_id, input.event_id);
    EXPECT_EQ(output.symbol, input.symbol);
    EXPECT_DOUBLE_EQ(output.price, input.price);
    EXPECT_DOUBLE_EQ(output.volume, input.volume);
    EXPECT_EQ(output.conditions, input.conditions);
    EXPECT_EQ(output.received_timestamp_unix_ms, input.received_timestamp_unix_ms);
}

}  // namespace
