#include "arrakis/news/aggregation.hpp"
#include "arrakis/news/market_features.hpp"
#include "arrakis/news/xlk_membership.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

TEST(NewsPointInTimeTest, ExcludesArticlesAfterCutoff) {
    arrakis::news::Article before; before.article_id = "before"; before.published_at_unix_ms = 100;
    arrakis::news::Article after = before; after.article_id = "after"; after.published_at_unix_ms = 101;
    EXPECT_TRUE(arrakis::news::eligible_at_cutoff(before, 100));
    EXPECT_FALSE(arrakis::news::eligible_at_cutoff(after, 100));
}

TEST(NewsAggregationTest, IsDeterministicAndProducesFixedVector) {
    arrakis::news::EnrichedArticle item;
    item.article.article_id = "article-1"; item.article.published_at_unix_ms = 100; item.article.normalized_content_hash = "hash"; item.article.source_id = "source"; item.sector_related = true;
    item.feature.article_id = "article-1"; item.feature.positive_probability = 0.7; item.feature.neutral_probability = 0.2; item.feature.negative_probability = 0.1; item.feature.sentiment_score = 0.6; item.feature.pooled_embedding = {1, 2, 3};
    const auto first = arrakis::news::aggregate_daily("2026-07-28", 200, {item});
    const auto second = arrakis::news::aggregate_daily("2026-07-28", 200, {item});
    EXPECT_EQ(first.values.size(), 27);
    EXPECT_EQ(first.to_json(), second.to_json());
    EXPECT_DOUBLE_EQ(first.values[4], 0.6);
    EXPECT_EQ(first.coverage_status, "complete");
}

TEST(NewsAggregationTest, EmptyNewsIsExplicit) {
    const auto result = arrakis::news::aggregate_daily("2026-07-28", 200, {});
    EXPECT_EQ(result.coverage_status, "empty");
    EXPECT_EQ(result.values[0], 0.0);
}

TEST(MarketFeatureTest, UsesRealVolatilityAndNonProxyRsi) {
    std::vector<arrakis::news::MarketDay> xlk_days;
    std::vector<arrakis::news::MarketDay> spy_days;
    for (int index = 0; index <= 14; ++index) {
        const auto date = "2020-01-" + std::string{index < 9 ? "0" : ""} + std::to_string(index + 1);
        const auto xlk_close = index == 14 ? 110.0 : 100.0;
        xlk_days.push_back({date, xlk_close, 1000.0});
        spy_days.push_back({date, 200.0, 2000.0});
    }

    const auto result = arrakis::news::market_feature_vector(xlk_days, spy_days, "2020-01-15");
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 9U);
    EXPECT_DOUBLE_EQ((*result)[6], 100.0);  // RSI reaches 100 with no losses.

    const auto last_log_return = std::log(1.1);
    const auto mean = last_log_return / 6.0;
    const auto expected_volatility = std::sqrt(
        ((last_log_return - mean) * (last_log_return - mean) + 5.0 * mean * mean) / 5.0);
    EXPECT_NEAR((*result)[3], expected_volatility, 1.0e-12);
    EXPECT_NE((*result)[6], std::clamp(50.0 + 10.0 * (*result)[0], 0.0, 100.0));
}

TEST(MarketFeatureTest, ConstantPricesProduceNeutralRsiAndZeroVolatility) {
    std::vector<arrakis::news::MarketDay> xlk_days;
    std::vector<arrakis::news::MarketDay> spy_days;
    for (int index = 0; index <= 14; ++index) {
        const auto date = "2020-02-" + std::string{index < 9 ? "0" : ""} + std::to_string(index + 1);
        xlk_days.push_back({date, 100.0, 1000.0});
        spy_days.push_back({date, 200.0, 2000.0});
    }
    const auto result = arrakis::news::market_feature_vector(xlk_days, spy_days, "2020-02-15");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ((*result)[3], 0.0);
    EXPECT_DOUBLE_EQ((*result)[6], 50.0);
}

namespace {
arrakis::news::EnrichedArticle article_at(std::string id, std::int64_t published_at_unix_ms) {
    arrakis::news::EnrichedArticle item;
    item.article.article_id = id;
    item.article.published_at_unix_ms = published_at_unix_ms;
    item.article.normalized_content_hash = "hash";
    item.article.source_id = "source";
    item.sector_related = true;
    item.holding_related = true;
    item.feature.article_id = std::move(id);
    item.feature.positive_probability = 0.5;
    item.feature.neutral_probability = 0.3;
    item.feature.negative_probability = 0.2;
    item.feature.sentiment_score = 0.3;
    item.feature.pooled_embedding = {1, 2, 3};
    return item;
}
}  // namespace

TEST(NewsPointInTimeTest, WindowBoundExcludesArticlesBeforeTheTradingDay) {
    const auto item = article_at("in-window", 500);
    EXPECT_TRUE(arrakis::news::eligible_in_window(item.article, 400, 600));
    EXPECT_FALSE(arrakis::news::eligible_in_window(item.article, 501, 600));
    EXPECT_FALSE(arrakis::news::eligible_in_window(item.article, 400, 499));
    // The lower bound is inclusive, so an article published exactly at the
    // window start belongs to that day.
    EXPECT_TRUE(arrakis::news::eligible_in_window(item.article, 500, 600));
}

TEST(NewsAggregationTest, WindowStartBoundsTheAggregateToOneTradingDay) {
    // Train/serve parity: build_xlk_combined_dataset groups training articles by
    // their own calendar date, so a training row aggregates exactly one day.
    // news-ingestion republishes NEWS_POLL_LOOKBACK_DAYS of history each run, so
    // without a lower bound the live article_count counts several days.
    const std::vector<arrakis::news::EnrichedArticle> articles{
        article_at("two-days-ago", 100),
        article_at("yesterday", 200),
        article_at("today-morning", 320),
        article_at("today-noon", 360),
    };

    const auto unbounded = arrakis::news::aggregate_daily("2026-07-28", 400, articles);
    EXPECT_DOUBLE_EQ(unbounded.values[0], 4.0);

    const auto bounded = arrakis::news::aggregate_daily("2026-07-28", 400, articles, 300);
    EXPECT_DOUBLE_EQ(bounded.values[0], 2.0);
    // abnormal_news_volume tracks the same count and must move with it.
    EXPECT_DOUBLE_EQ(bounded.values[11], 2.0);
}

TEST(NewsAggregationTest, DefaultWindowStartPreservesBatchBuilderBehaviour) {
    // The batch builder calls the three-argument overload. Its results must not
    // change: a default of 0 means "no lower bound".
    const std::vector<arrakis::news::EnrichedArticle> articles{
        article_at("a", 1), article_at("b", 2)};
    const auto implicit_bound = arrakis::news::aggregate_daily("2026-07-28", 400, articles);
    const auto explicit_zero = arrakis::news::aggregate_daily("2026-07-28", 400, articles, 0);
    EXPECT_EQ(implicit_bound.to_json(), explicit_zero.to_json());
}

// --- Point-in-time XLK membership -------------------------------------------

namespace {

// Three quarterly snapshots. ZZZ is present in the first two and then vanishes
// (a real deletion). GAPCO is present in the first, absent in the second, and
// back in the third (CSCO behaves exactly like this in the real file).
constexpr std::string_view kHistory =
    "symbol,effective_from,effective_to,weight,source\n"
    "AAPL,2019-09-30,2099-12-31,17.5,SEC-NPORT-2019q4\n"
    "AAPL,2019-09-30,2099-12-31,17.5,SEC-NPORT-2019q4\n"
    "ZZZ,2019-09-30,2099-12-31,2.0,SEC-NPORT-2019q4\n"
    "GAPCO,2019-09-30,2099-12-31,1.0,SEC-NPORT-2019q4\n"
    "AAPL,2019-12-31,2099-12-31,19.7,SEC-NPORT-2020q1\n"
    "ZZZ,2019-12-31,2099-12-31,1.5,SEC-NPORT-2020q1\n"
    "AAPL,2020-03-31,2099-12-31,19.4,SEC-NPORT-2020q2\n"
    "GAPCO,2020-03-31,2099-12-31,1.2,SEC-NPORT-2020q2\n";

arrakis::news::XlkMembershipResolver resolver() {
    return arrakis::news::XlkMembershipResolver::from_csv_text(kHistory);
}

std::vector<std::string> symbols_on(const std::string_view date) {
    const auto history = resolver();  // must outlive the returned reference
    std::vector<std::string> output;
    for (const auto& member : history.constituents_on(date)) output.push_back(member.symbol);
    return output;
}

}  // namespace

TEST(XlkMembershipTest, ReturnsNothingBeforeTheFirstSnapshot) {
    // No survivorship fallback to current holdings: the answer is "unknown/empty",
    // never "whatever is in the fund today".
    EXPECT_TRUE(symbols_on("2019-09-29").empty());
    EXPECT_TRUE(symbols_on("2015-01-01").empty());
    EXPECT_FALSE(resolver().held_on("AAPL", "2019-09-29"));
    EXPECT_FALSE(resolver().governing_snapshot("2019-09-29").has_value());
    EXPECT_EQ(resolver().first_snapshot_date(), "2019-09-30");
}

TEST(XlkMembershipTest, SnapshotAppliesInclusiveFromItsEffectiveDate) {
    EXPECT_EQ(symbols_on("2019-09-30"), (std::vector<std::string>{"AAPL", "GAPCO", "ZZZ"}));
    EXPECT_EQ(resolver().governing_snapshot("2019-09-30").value(), "2019-09-30");
    EXPECT_EQ(resolver().governing_snapshot("2019-12-30").value(), "2019-09-30");
}

TEST(XlkMembershipTest, StrictLookupDoesNotUseSameDayFiling) {
    const auto history = resolver();
    EXPECT_TRUE(history.held_on("AAPL", "2019-09-30"));
    EXPECT_FALSE(history.held_strictly_before("AAPL", "2019-09-30"));
    EXPECT_EQ(history.governing_snapshot_strictly_before("2019-09-30"), std::nullopt);
    EXPECT_TRUE(history.held_strictly_before("AAPL", "2019-12-30"));
    EXPECT_EQ(
        history.governing_snapshot_strictly_before("2019-12-30").value(),
        "2019-09-30"
    );
    EXPECT_TRUE(history.held_strictly_before("ZZZ", "2020-01-02"));
    EXPECT_FALSE(history.held_strictly_before("GAPCO", "2020-01-02"));
}

TEST(XlkMembershipTest, NextSnapshotSupersedesOnItsOwnDate) {
    EXPECT_EQ(resolver().governing_snapshot("2019-12-31").value(), "2019-12-31");
    EXPECT_DOUBLE_EQ(resolver().weight_on("AAPL", "2019-12-30").value(), 17.5);
    EXPECT_DOUBLE_EQ(resolver().weight_on("AAPL", "2019-12-31").value(), 19.7);
}

TEST(XlkMembershipTest, DeletionTakesEffectAtTheNextSnapshotNotTheNextRowForThatSymbol) {
    // ZZZ's last row is 2019-12-31 with the 2099-12-31 sentinel. Trusting that
    // column, or extending the last per-symbol row forward, would keep ZZZ in
    // the fund forever.
    EXPECT_TRUE(resolver().held_on("ZZZ", "2019-12-31"));
    EXPECT_TRUE(resolver().held_on("ZZZ", "2020-03-30"));
    EXPECT_FALSE(resolver().held_on("ZZZ", "2020-03-31"));
    EXPECT_FALSE(resolver().held_on("ZZZ", "2026-08-08"));
}

TEST(XlkMembershipTest, ReentryLeavesARealGap) {
    // GAPCO is missing from the 2019-12-31 filing, so it is not a constituent
    // for that entire quarter even though it returns later.
    EXPECT_TRUE(resolver().held_on("GAPCO", "2019-09-30"));
    EXPECT_FALSE(resolver().held_on("GAPCO", "2019-12-31"));
    EXPECT_FALSE(resolver().held_on("GAPCO", "2020-03-30"));
    EXPECT_TRUE(resolver().held_on("GAPCO", "2020-03-31"));
}

TEST(XlkMembershipTest, LastSnapshotIsCarriedForwardAndFlagged) {
    EXPECT_EQ(resolver().last_snapshot_date(), "2020-03-31");
    EXPECT_EQ(symbols_on("2026-08-08"), (std::vector<std::string>{"AAPL", "GAPCO"}));
    EXPECT_TRUE(resolver().is_extrapolated_forward("2020-03-31"));
    EXPECT_TRUE(resolver().is_extrapolated_forward("2026-08-08"));
    EXPECT_FALSE(resolver().is_extrapolated_forward("2020-03-30"));
}

TEST(XlkMembershipTest, IdenticalDuplicateRowsAreDedupedAndConflictsRejected) {
    EXPECT_EQ(std::ranges::count(symbols_on("2019-09-30"), "AAPL"), 1);
    EXPECT_THROW(
        static_cast<void>(arrakis::news::XlkMembershipResolver::from_csv_text(
            "symbol,effective_from,effective_to,weight,source\n"
            "AAPL,2019-09-30,2099-12-31,17.5,a\n"
            "AAPL,2019-09-30,2099-12-31,18.5,a\n"
        )),
        arrakis::news::MembershipDataError
    );
}

TEST(XlkMembershipTest, RejectsAnEffectiveToThatIsNotTheIgnoredSentinel) {
    // effective_to carries no information today. If it ever does, fail loudly
    // rather than keep ignoring it.
    EXPECT_THROW(
        static_cast<void>(arrakis::news::XlkMembershipResolver::from_csv_text(
            "symbol,effective_from,effective_to,weight,source\n"
            "AAPL,2019-09-30,2020-06-30,17.5,a\n"
        )),
        arrakis::news::MembershipDataError
    );
}

TEST(XlkMembershipTest, RejectsMalformedInput) {
    EXPECT_THROW(
        static_cast<void>(arrakis::news::XlkMembershipResolver::from_csv_text("wrong,header\n")),
        arrakis::news::MembershipDataError
    );
    EXPECT_THROW(
        static_cast<void>(arrakis::news::XlkMembershipResolver::from_csv_text(
            "symbol,effective_from,effective_to,weight,source\n"
        )),
        arrakis::news::MembershipDataError
    );
    EXPECT_THROW(
        static_cast<void>(arrakis::news::XlkMembershipResolver::from_csv_text(
            "symbol,effective_from,effective_to,weight,source\n"
            "AAPL,2019-9-30,2099-12-31,17.5,a\n"
        )),
        arrakis::news::MembershipDataError
    );
}

TEST(XlkMembershipTest, FiltersFilingArtifactTickersFromPolling) {
    EXPECT_TRUE(arrakis::news::is_pollable_ticker("MSFT"));
    EXPECT_TRUE(arrakis::news::is_pollable_ticker("NVDA"));
    EXPECT_FALSE(arrakis::news::is_pollable_ticker("HPE-PC"));
    EXPECT_FALSE(arrakis::news::is_pollable_ticker("ORCL-PD"));
    EXPECT_FALSE(arrakis::news::is_pollable_ticker(""));
}

TEST(XlkMembershipTest, ResolvesTheRealHoldingsFileWhenAvailable) {
    const auto path = arrakis::news::XlkMembershipResolver::default_history_path();
    if (!std::filesystem::exists(path)) GTEST_SKIP() << "holdings history not found at " << path;
    const auto history = arrakis::news::XlkMembershipResolver::from_csv(path);
    EXPECT_EQ(history.first_snapshot_date(), "2019-09-30");
    EXPECT_TRUE(history.constituents_on("2019-09-29").empty());
    EXPECT_EQ(history.constituents_on("2019-09-30").size(), 46);  // 47 rows, one AAPL duplicate
    EXPECT_TRUE(history.held_on("MSFT", "2019-09-30"));
    // CSCO is absent from the 2021-03-31 through 2021-12-31 filings.
    EXPECT_TRUE(history.held_on("CSCO", "2020-12-31"));
    EXPECT_FALSE(history.held_on("CSCO", "2021-06-30"));
    EXPECT_TRUE(history.held_on("CSCO", "2022-03-31"));
    // V and MA leave the fund after the 2022-12-31 filing and must not persist.
    EXPECT_TRUE(history.held_on("V", "2022-12-31"));
    EXPECT_FALSE(history.held_on("V", "2023-03-31"));
    EXPECT_FALSE(history.held_on("MA", "2023-03-31"));
}
