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
[[nodiscard]] DailyNewsFeatures aggregate_daily(std::string trading_date, std::int64_t cutoff_unix_ms,
                                                std::vector<EnrichedArticle> articles);

}  // namespace arrakis::news
