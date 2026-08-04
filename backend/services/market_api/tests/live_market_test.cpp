#include "arrakis/market_api/live_market.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace {

arrakis::market::NormalizedTrade trade(
    std::string id, double price, double volume, std::uint64_t timestamp_ms) {
    return {
        std::move(id),
        "finnhub",
        "XLK",
        price,
        volume,
        timestamp_ms,
        {},
        {},
        timestamp_ms + 1U,
    };
}

TEST(LiveMarketStoreTest, BuildsInMemoryBarsAndRejectsDuplicates) {
    arrakis::market_api::LiveMarketStore store(ARRAKIS_TEST_ETF_UNIVERSE);

    EXPECT_TRUE(store.apply(trade("one", 100.0, 5.0, 1710000000000U)));
    EXPECT_TRUE(store.apply(trade("two", 102.0, 3.0, 1710000010000U)));
    EXPECT_FALSE(store.apply(trade("two", 999.0, 3.0, 1710000010000U)));

    const auto one_minute = store.latest("XLK", "1m");
    ASSERT_TRUE(one_minute.has_value());
    EXPECT_DOUBLE_EQ(one_minute->open, 100.0);
    EXPECT_DOUBLE_EQ(one_minute->high, 102.0);
    EXPECT_DOUBLE_EQ(one_minute->low, 100.0);
    EXPECT_DOUBLE_EQ(one_minute->close, 102.0);
    EXPECT_DOUBLE_EQ(one_minute->volume, 8.0);
    EXPECT_EQ(one_minute->trade_count, 2U);

    const auto bars = store.bars("XLK", "1m", std::nullopt, std::nullopt, 120);
    ASSERT_EQ(bars.size(), 1U);
    EXPECT_FALSE(bars.front().finalized);
}

TEST(LiveMarketStoreTest, RollsBarsWithoutDatabasePersistence) {
    arrakis::market_api::LiveMarketStore store(ARRAKIS_TEST_ETF_UNIVERSE);
    EXPECT_TRUE(store.apply(trade("minute-one", 100.0, 1.0, 1710000000000U)));
    EXPECT_TRUE(store.apply(trade("minute-two", 105.0, 2.0, 1710000060000U)));

    const auto bars = store.bars("XLK", "1m", std::nullopt, std::nullopt, 120);
    ASSERT_EQ(bars.size(), 2U);
    EXPECT_TRUE(bars.front().finalized);
    EXPECT_FALSE(bars.back().finalized);
    EXPECT_DOUBLE_EQ(bars.back().close, 105.0);
}

}  // namespace
