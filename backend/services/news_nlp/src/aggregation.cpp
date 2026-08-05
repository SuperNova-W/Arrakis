#include "arrakis/news/aggregation.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace arrakis::news {
namespace {
const std::vector<std::string> names = [] {
    std::vector<std::string> output;
    output.reserve(kNewsFeatureCount);
    for (const auto name : kNewsFeatureNames) output.emplace_back(name);
    return output;
}();

double mean(const std::vector<double>& values) { return values.empty() ? 0.0 : std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size()); }
}

bool eligible_at_cutoff(const Article& article, std::int64_t cutoff_unix_ms) {
    return article.published_at_unix_ms > 0 && article.published_at_unix_ms <= cutoff_unix_ms;
}

DailyNewsFeatures aggregate_daily(std::string trading_date, std::int64_t cutoff_unix_ms, std::vector<EnrichedArticle> articles) {
    articles.erase(std::remove_if(articles.begin(), articles.end(), [&](const auto& item) { return !eligible_at_cutoff(item.article, cutoff_unix_ms); }), articles.end());
    DailyNewsFeatures output; output.trading_date = std::move(trading_date); output.cutoff_unix_ms = cutoff_unix_ms; output.feature_names = names; output.values.assign(names.size(), 0.0);
    if (articles.empty()) return output;
    std::vector<double> sentiments, positive, negative, decayed, weighted, novelty, source_weighted;
    sentiments.reserve(articles.size()); positive.reserve(articles.size()); negative.reserve(articles.size());
    double embedding[8]{}; int macro = 0, sector = 0, holdings = 0;
    std::int64_t latest = 0;
    for (const auto& item : articles) {
        const auto& feature = item.feature;
        sentiments.push_back(feature.sentiment_score); positive.push_back(feature.positive_probability); negative.push_back(feature.negative_probability); novelty.push_back(item.article.normalized_content_hash.empty() ? 0.0 : 1.0);
        const double age_hours = std::max(0.0, static_cast<double>(cutoff_unix_ms - item.article.published_at_unix_ms) / 3600000.0);
        const double decay = std::exp(-age_hours / 12.0);
        decayed.push_back(feature.sentiment_score * decay); weighted.push_back(feature.sentiment_score * item.entity_weight); source_weighted.push_back(feature.sentiment_score * (item.article.source_id == "fixture" ? 0.5 : 1.0));
        latest = std::max(latest, item.article.published_at_unix_ms); macro += item.macro_related ? 1 : 0; sector += item.sector_related ? 1 : 0; holdings += item.holding_related ? 1 : 0;
        for (std::size_t index = 0; index < 8 && index < feature.pooled_embedding.size(); ++index) embedding[index] += feature.pooled_embedding[index];
    }
    const auto average = mean(sentiments); double variance = 0.0; for (const auto value : sentiments) variance += (value - average) * (value - average); variance /= static_cast<double>(sentiments.size());
    output.latest_article_unix_ms = latest; output.coverage_status = "complete";
    output.values[0] = static_cast<double>(articles.size()); output.values[1] = mean(positive); output.values[2] = std::max(0.0, 1.0 - output.values[1] - mean(negative)); output.values[3] = mean(negative); output.values[4] = average; output.values[5] = *std::max_element(positive.begin(), positive.end()); output.values[6] = *std::max_element(negative.begin(), negative.end()); output.values[7] = std::sqrt(variance); output.values[8] = mean(decayed); output.values[9] = mean(weighted); output.values[10] = holdings == 0 ? 0.0 : mean(weighted) * static_cast<double>(holdings) / static_cast<double>(articles.size()); output.values[11] = static_cast<double>(articles.size()); output.values[12] = mean(novelty); output.values[13] = mean(source_weighted); output.values[14] = std::sqrt(variance); output.values[15] = static_cast<double>(sector) / static_cast<double>(articles.size()); output.values[16] = static_cast<double>(macro); output.values[17] = 1.0; output.values[18] = static_cast<double>(cutoff_unix_ms - latest) / 3600000.0; for (std::size_t index = 0; index < 8; ++index) output.values[19 + index] = embedding[index] / static_cast<double>(articles.size());
    return output;
}

std::string DailyNewsFeatures::to_json() const {
    boost::json::array values_json;
    for (const auto value : values) values_json.push_back(value);
    boost::json::object result{{"schema", feature_schema_hash}, {"values", values_json}};
    for (std::size_t index = 0; index < feature_names.size() && index < values.size(); ++index) result[feature_names[index]] = values[index];
    return boost::json::serialize(result);
}

std::string DailyNewsFeatures::to_combined_json(std::span<const double> market_values) const {
    const auto combined = combine_feature_values(market_values, values);
    boost::json::array values_json;
    for (const auto value : combined) values_json.push_back(value);
    const auto combined_names = combined_feature_names();
    boost::json::object result{{"schema", kCombinedFeatureSchemaHash}, {"values", values_json}};
    for (std::size_t index = 0; index < combined_names.size(); ++index) result[combined_names[index]] = combined[index];
    return boost::json::serialize(result);
}
}  // namespace arrakis::news
