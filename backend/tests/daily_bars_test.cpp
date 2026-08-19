#include "arrakis/database/daily_bars.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using arrakis::database::DailyMarketBar;

// A real row from backend/data/history/XLK.csv.
constexpr const char* kXlkRow =
    "\"XLK\",1469712600,23.155000686645508,23.209999084472656,23.049999237060547,23.139999389648438,12658600";

}  // namespace

TEST(DailyBarTradingDate, MatchesTrainingDatasetDerivationInDaylightTime) {
    // 1469712600 == 2016-07-28 13:30:00Z == 2016-07-28 09:30:00 America/New_York.
    EXPECT_EQ(arrakis::database::trading_date_from_unix_seconds(1469712600), "2016-07-28");
}

TEST(DailyBarTradingDate, MatchesTrainingDatasetDerivationInStandardTime) {
    // 1609770600 == 2021-01-04 14:30:00Z == 2021-01-04 09:30:00 America/New_York.
    EXPECT_EQ(arrakis::database::trading_date_from_unix_seconds(1609770600), "2021-01-04");
}

TEST(DailyBarTradingDate, SessionOpenEpochsAgreeBetweenUtcAndNewYork) {
    // Every history row is stamped at the 09:30 America/New_York session open,
    // which is 13:30Z (daylight) or 14:30Z (standard).  In both cases the UTC
    // calendar date used by build_xlk_combined_dataset.cpp equals the
    // America/New_York trading date, so the backfill keys line up with the
    // training dataset.
    EXPECT_EQ(arrakis::database::trading_date_from_unix_seconds(1469712600 + 0), "2016-07-28");
    EXPECT_EQ(arrakis::database::trading_date_from_unix_seconds(1469712600 + 86400), "2016-07-29");
    // A midnight-UTC epoch would be the previous New York day; assert we are not
    // silently handed one of those.
    EXPECT_EQ(1469712600 % 86400, 48600);   // 13:30 UTC
    EXPECT_EQ(1609770600 % 86400, 52200);   // 14:30 UTC
}

TEST(DailyBarCsv, ParsesQuotedSymbolAndOhlcv) {
    const auto record = arrakis::database::parse_daily_bar_csv_line(kXlkRow);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->symbol, "XLK");
    EXPECT_EQ(record->trading_date, "2016-07-28");
    EXPECT_DOUBLE_EQ(record->close, 23.139999389648438);
    EXPECT_DOUBLE_EQ(record->volume, 12658600.0);
    EXPECT_LE(record->low, record->open);
    EXPECT_LE(record->open, record->high);
}

TEST(DailyBarCsv, RejectsHeaderBlankAndMalformedRows) {
    EXPECT_FALSE(arrakis::database::parse_daily_bar_csv_line("symbol,timestamp_utc,open,high,low,close,volume")
                     .has_value());
    EXPECT_FALSE(arrakis::database::parse_daily_bar_csv_line("").has_value());
    EXPECT_FALSE(arrakis::database::parse_daily_bar_csv_line("\"XLK\",1469712600,1,2").has_value());
    // close outside [low, high]
    EXPECT_FALSE(arrakis::database::parse_daily_bar_csv_line("\"XLK\",1469712600,10,11,9,99,100").has_value());
    // negative volume
    EXPECT_FALSE(arrakis::database::parse_daily_bar_csv_line("\"XLK\",1469712600,10,11,9,10,-1").has_value());
}

TEST(DailyBarSessionClose, UsesTwentyHundredUtcInDaylightAndTwentyOneInStandard) {
    EXPECT_EQ(arrakis::database::session_close_unix_ms("2021-07-01"), 1625169600000LL);
    EXPECT_EQ(arrakis::database::session_close_unix_ms("2021-01-04"), 1609794000000LL);
    EXPECT_EQ(arrakis::database::session_close_unix_ms("2016-07-28"), 1469736000000LL);
    EXPECT_EQ(arrakis::database::session_close_unix_ms("2016-12-06"), 1481058000000LL);
}

TEST(DailyBarSessionClose, HandlesDaylightTransitionBoundaries) {
    // 2021 DST: starts Sunday 2021-03-14, ends Sunday 2021-11-07.
    EXPECT_EQ(arrakis::database::session_close_unix_ms("2021-03-12") % 86400000LL, 21 * 3600000LL);
    EXPECT_EQ(arrakis::database::session_close_unix_ms("2021-03-15") % 86400000LL, 20 * 3600000LL);
    EXPECT_EQ(arrakis::database::session_close_unix_ms("2021-11-05") % 86400000LL, 20 * 3600000LL);
    EXPECT_EQ(arrakis::database::session_close_unix_ms("2021-11-08") % 86400000LL, 21 * 3600000LL);
}

TEST(DailyBarCutoff, DailyBarIsInvisibleUntilItsSessionCloses) {
    const auto close = arrakis::database::session_close_unix_ms("2021-07-01");
    EXPECT_TRUE(arrakis::database::daily_bar_visible_at("2021-07-01", close));
    EXPECT_TRUE(arrakis::database::daily_bar_visible_at("2021-07-01", close + 1));
    EXPECT_FALSE(arrakis::database::daily_bar_visible_at("2021-07-01", close - 1));
    EXPECT_FALSE(arrakis::database::daily_bar_visible_at("2021-07-01", 0));
}

TEST(DailyBarCutoff, FilterKeepsOnlyClosedSessionsAndSortsAscending) {
    const std::vector<DailyMarketBar> bars{
        {"2021-07-02", 3.0, 30.0},
        {"2021-06-30", 1.0, 10.0},
        {"2021-07-01", 2.0, 20.0},
    };
    // Cutoff is 2021-07-01 12:00 New York, i.e. mid-session.
    const auto cutoff = arrakis::database::session_close_unix_ms("2021-07-01") - 4 * 3600000LL;
    const auto filtered = arrakis::database::filter_completed_sessions(bars, cutoff);
    ASSERT_EQ(filtered.size(), 1U);
    EXPECT_EQ(filtered.front().trading_date, "2021-06-30");

    const auto after_close = arrakis::database::filter_completed_sessions(
        bars, arrakis::database::session_close_unix_ms("2021-07-01"));
    ASSERT_EQ(after_close.size(), 2U);
    EXPECT_EQ(after_close[0].trading_date, "2021-06-30");
    EXPECT_EQ(after_close[1].trading_date, "2021-07-01");
}

TEST(DailyBarMerge, AppendsInProgressSessionAndPrefersCompletedBar) {
    const std::vector<DailyMarketBar> completed{{"2021-06-30", 1.0, 10.0}};
    const std::vector<DailyMarketBar> intraday{{"2021-07-01", 2.5, 5.0}};
    const auto merged = arrakis::database::merge_daily_and_intraday(completed, intraday);
    ASSERT_EQ(merged.size(), 2U);
    EXPECT_EQ(merged[0].trading_date, "2021-06-30");
    EXPECT_EQ(merged[1].trading_date, "2021-07-01");
    EXPECT_DOUBLE_EQ(merged[1].close, 2.5);

    // A partial stream reconstruction must never replace the true daily bar.
    const std::vector<DailyMarketBar> overlapping{{"2021-06-30", 9.9, 0.1}};
    const auto kept = arrakis::database::merge_daily_and_intraday(completed, overlapping);
    ASSERT_EQ(kept.size(), 1U);
    EXPECT_DOUBLE_EQ(kept[0].close, 1.0);
    EXPECT_DOUBLE_EQ(kept[0].volume, 10.0);
}

TEST(DailyBarMerge, NeverAdmitsBarsFromAfterTheCutoff) {
    // End-to-end shape of PostgresPool::daily_market_bars: the SQL range scan is
    // simulated here, the leakage boundary is enforced by these pure functions.
    const std::vector<DailyMarketBar> from_daily_table{
        {"2021-06-29", 1.0, 10.0}, {"2021-06-30", 2.0, 20.0}, {"2021-07-01", 3.0, 30.0}};
    const std::vector<DailyMarketBar> from_stream{{"2021-07-01", 2.9, 4.0}};
    const auto cutoff = arrakis::database::session_close_unix_ms("2021-07-01") - 60000LL;
    const auto result = arrakis::database::merge_daily_and_intraday(
        arrakis::database::filter_completed_sessions(from_daily_table, cutoff), from_stream);
    ASSERT_EQ(result.size(), 3U);
    EXPECT_EQ(result.back().trading_date, "2021-07-01");
    // The 2021-07-01 close comes from the still-open stream, not from the
    // end-of-day row that is only known after 16:00 New York.
    EXPECT_DOUBLE_EQ(result.back().close, 2.9);
    EXPECT_DOUBLE_EQ(result.back().volume, 4.0);
    for (const auto& bar : result) {
        EXPECT_LE(bar.trading_date, std::string{"2021-07-01"});
    }
}
