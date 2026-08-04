#include "arrakis/news/aggregation.hpp"

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
