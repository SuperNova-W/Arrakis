#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace arrakis::news {

inline constexpr std::string_view kCombinedFeatureSchemaHash{"xlk-combined-features-v2"};
inline constexpr std::string_view kNewsFeatureSchemaHash{"xlk-news-features-v1"};
inline constexpr std::string_view kLogitsOnlyNewsFeatureSchemaHash{
    "xlk-news-logits-only-features-v3"
};
inline constexpr std::string_view kLogitsOnlyCombinedFeatureSchemaHash{
    "xlk-logits-only-combined-features-v3"
};

inline constexpr std::array<std::string_view, 9> kMarketFeatureNames{
    "ret_1", "ret_3", "ret_6", "volatility_6", "volume_mean_6", "rel_volume",
    "rsi_14", "spy_ret_1", "sector_spy_diff"};

inline constexpr std::array<std::string_view, 27> kNewsFeatureNames{
    "article_count", "positive_proportion", "neutral_proportion", "negative_proportion",
    "average_sentiment", "max_positive_sentiment", "max_negative_sentiment", "sentiment_stddev",
    "time_decayed_sentiment", "entity_weighted_sentiment", "holding_sentiment", "abnormal_news_volume",
    "article_novelty", "source_weighted_sentiment", "cross_article_disagreement", "sector_breadth",
    "macro_news_count", "news_coverage", "news_freshness_hours", "embedding_0", "embedding_1",
    "embedding_2", "embedding_3", "embedding_4", "embedding_5", "embedding_6", "embedding_7"};

inline constexpr std::size_t kMarketFeatureCount = kMarketFeatureNames.size();
inline constexpr std::size_t kNewsFeatureCount = kNewsFeatureNames.size();
inline constexpr std::size_t kCombinedFeatureCount = kMarketFeatureCount + kNewsFeatureCount;

// The frozen FinBERT artifact currently exposes sentiment logits only. Keep
// this contract separate from the legacy 27-column streaming shape so a batch
// dataset cannot silently claim to contain embeddings that are all zero.
inline constexpr std::array<std::string_view, 19> kLogitsOnlyNewsFeatureNames{
    "article_count", "positive_proportion", "neutral_proportion", "negative_proportion",
    "average_sentiment", "max_positive_sentiment", "max_negative_sentiment", "sentiment_stddev",
    "time_decayed_sentiment", "entity_weighted_sentiment", "holding_sentiment", "abnormal_news_volume",
    "article_novelty", "source_weighted_sentiment", "cross_article_disagreement", "sector_breadth",
    "macro_news_count", "news_coverage", "news_freshness_hours"};

inline constexpr std::size_t kLogitsOnlyNewsFeatureCount = kLogitsOnlyNewsFeatureNames.size();
inline constexpr std::size_t kLogitsOnlyCombinedFeatureCount =
    kMarketFeatureCount + kLogitsOnlyNewsFeatureCount;

[[nodiscard]] inline std::vector<std::string> combined_feature_names() {
    std::vector<std::string> names;
    names.reserve(kCombinedFeatureCount);
    for (const auto name : kMarketFeatureNames) names.emplace_back(name);
    for (const auto name : kNewsFeatureNames) names.emplace_back(name);
    return names;
}

[[nodiscard]] inline std::vector<std::string> logits_only_combined_feature_names() {
    std::vector<std::string> names;
    names.reserve(kLogitsOnlyCombinedFeatureCount);
    for (const auto name : kMarketFeatureNames) names.emplace_back(name);
    for (const auto name : kLogitsOnlyNewsFeatureNames) names.emplace_back(name);
    return names;
}

[[nodiscard]] inline std::vector<double> combine_feature_values(
    std::span<const double> market_values, std::span<const double> news_values) {
    if (market_values.size() != kMarketFeatureCount || news_values.size() != kNewsFeatureCount) {
        throw std::invalid_argument{"XLK combined feature vector has an invalid component length"};
    }
    std::vector<double> values;
    values.reserve(kCombinedFeatureCount);
    values.insert(values.end(), market_values.begin(), market_values.end());
    values.insert(values.end(), news_values.begin(), news_values.end());
    return values;
}

}  // namespace arrakis::news
