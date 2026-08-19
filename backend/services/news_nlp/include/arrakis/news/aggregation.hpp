#pragma once

#include "arrakis/serialization/news_serialization.hpp"
#include "arrakis/news/feature_schema.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace arrakis::news {

struct EnrichedArticle final {
    Article article;
    EnrichedFeature feature;
    double entity_weight{1.0};
    bool holding_related{};
    bool macro_related{};
    bool sector_related{};
};

struct DailyNewsFeatures final {
    std::string symbol{"XLK"};
    std::string trading_date;
    std::int64_t cutoff_unix_ms{};
    std::int64_t latest_article_unix_ms{};
    std::string coverage_status{"empty"};
    std::vector<std::string> feature_names;
    std::vector<double> values;
    std::string feature_schema_hash{std::string{kNewsFeatureSchemaHash}};
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] std::string to_combined_json(std::span<const double> market_values) const;
};

[[nodiscard]] bool eligible_at_cutoff(const Article& article, std::int64_t cutoff_unix_ms);

// `window_start_unix_ms` is an INCLUSIVE lower bound on publication time.
//
// It exists for train/serve parity. build_xlk_combined_dataset groups training
// articles by their own calendar date, so every training row aggregates exactly
// one day of news. The streaming path instead consumes whatever news-ingestion
// republished, which is NEWS_POLL_LOOKBACK_DAYS deep (3 by default) -- so
// without a lower bound the live `article_count` counts roughly three days of
// articles against a model trained on one, and `abnormal_news_volume`,
// `news_coverage` and `news_freshness_hours` are distorted with it.
//
// The default of 0 keeps the historical single-argument behaviour for the batch
// builder, whose per-day grouping already bounds the window.
[[nodiscard]] bool eligible_in_window(const Article& article, std::int64_t window_start_unix_ms,
                                      std::int64_t cutoff_unix_ms);
[[nodiscard]] DailyNewsFeatures aggregate_daily(std::string trading_date, std::int64_t cutoff_unix_ms,
                                                std::vector<EnrichedArticle> articles,
                                                std::int64_t window_start_unix_ms = 0);

}  // namespace arrakis::news
