#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace arrakis::news {

struct Article final {
    std::string article_id;
    std::string canonical_url;
    std::string normalized_content_hash;
    std::string source_id;
    std::string headline;
    std::string body;
    std::int64_t published_at_unix_ms{};
    std::int64_t retrieved_at_unix_ms{};
    std::string language{"en"};
    std::vector<std::string> entity_ids;
};

struct EnrichedFeature final {
    std::string article_id;
    std::string model_version;
    std::string tokenizer_version;
    double positive_probability{};
    double neutral_probability{};
    double negative_probability{};
    double sentiment_score{};
    std::vector<double> pooled_embedding;
    double relevance_score{};
    std::int64_t prediction_cutoff_unix_ms{};
};

[[nodiscard]] std::vector<std::byte> serialize_article(const Article& article);
[[nodiscard]] Article deserialize_article(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> serialize_enriched_feature(const EnrichedFeature& feature);
[[nodiscard]] EnrichedFeature deserialize_enriched_feature(std::span<const std::byte> bytes);

}  // namespace arrakis::news
