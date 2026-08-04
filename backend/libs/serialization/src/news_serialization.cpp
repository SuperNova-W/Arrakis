#include "arrakis/serialization/news_serialization.hpp"

#include "news_article.pb.h"

#include <cstring>
#include <stdexcept>

namespace arrakis::news {
namespace {
template <typename Message>
std::vector<std::byte> encode(const Message& message) {
    std::string bytes;
    if (!message.SerializeToString(&bytes)) throw std::runtime_error("news protobuf serialization failed");
    std::vector<std::byte> output(bytes.size());
    std::memcpy(output.data(), bytes.data(), bytes.size());
    return output;
}
}

std::vector<std::byte> serialize_article(const Article& article) {
    market::events::v1::NewsArticle message;
    auto* metadata = message.mutable_metadata();
    metadata->set_event_id(article.article_id);
    metadata->set_event_time_unix_ms(article.published_at_unix_ms);
    metadata->set_produced_time_unix_ms(article.retrieved_at_unix_ms);
    metadata->set_producer("news-ingestion-v1");
    metadata->set_schema_version("news-article-v1");
    message.set_article_id(article.article_id);
    message.set_canonical_url(article.canonical_url);
    message.set_normalized_content_hash(article.normalized_content_hash);
    message.set_source_id(article.source_id);
    message.set_headline(article.headline);
    message.set_body(article.body);
    message.set_published_at_unix_ms(article.published_at_unix_ms);
    message.set_retrieved_at_unix_ms(article.retrieved_at_unix_ms);
    message.set_language(article.language);
    for (const auto& entity : article.entity_ids) message.add_entity_ids(entity);
    return encode(message);
}

Article deserialize_article(std::span<const std::byte> bytes) {
    market::events::v1::NewsArticle message;
    if (!message.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) throw std::runtime_error("invalid NewsArticle protobuf");
    if (message.article_id().empty() || message.canonical_url().empty() || message.source_id().empty() || message.published_at_unix_ms() <= 0) throw std::invalid_argument("NewsArticle missing required fields");
    Article result;
    result.article_id = message.article_id(); result.canonical_url = message.canonical_url(); result.normalized_content_hash = message.normalized_content_hash(); result.source_id = message.source_id(); result.headline = message.headline(); result.body = message.body(); result.published_at_unix_ms = message.published_at_unix_ms(); result.retrieved_at_unix_ms = message.retrieved_at_unix_ms(); result.language = message.language(); result.entity_ids.assign(message.entity_ids().begin(), message.entity_ids().end());
    return result;
}

std::vector<std::byte> serialize_enriched_feature(const EnrichedFeature& feature) {
    market::events::v1::NewsEnrichedFeature message;
    auto* metadata = message.mutable_metadata();
    metadata->set_event_id(feature.article_id);
    metadata->set_event_time_unix_ms(feature.prediction_cutoff_unix_ms);
    metadata->set_producer("news-enricher-v1");
    metadata->set_schema_version("news-enriched-feature-v1");
    message.set_article_id(feature.article_id); message.set_model_version(feature.model_version); message.set_tokenizer_version(feature.tokenizer_version);
    message.set_positive_probability(feature.positive_probability); message.set_neutral_probability(feature.neutral_probability); message.set_negative_probability(feature.negative_probability); message.set_sentiment_score(feature.sentiment_score); message.set_relevance_score(feature.relevance_score); message.set_prediction_cutoff_unix_ms(feature.prediction_cutoff_unix_ms);
    for (const auto value : feature.pooled_embedding) message.add_pooled_embedding(value);
    return encode(message);
}

EnrichedFeature deserialize_enriched_feature(std::span<const std::byte> bytes) {
    market::events::v1::NewsEnrichedFeature message;
    if (!message.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) throw std::runtime_error("invalid NewsEnrichedFeature protobuf");
    if (message.article_id().empty() || message.model_version().empty() || message.tokenizer_version().empty()) throw std::invalid_argument("NewsEnrichedFeature missing identity");
    const auto total = message.positive_probability() + message.neutral_probability() + message.negative_probability();
    if (message.positive_probability() < 0 || message.neutral_probability() < 0 || message.negative_probability() < 0 || total < 0.999 || total > 1.001) throw std::invalid_argument("NewsEnrichedFeature probabilities are invalid");
    EnrichedFeature result;
    result.article_id = message.article_id(); result.model_version = message.model_version(); result.tokenizer_version = message.tokenizer_version(); result.positive_probability = message.positive_probability(); result.neutral_probability = message.neutral_probability(); result.negative_probability = message.negative_probability(); result.sentiment_score = message.sentiment_score(); result.relevance_score = message.relevance_score(); result.prediction_cutoff_unix_ms = message.prediction_cutoff_unix_ms(); result.pooled_embedding.assign(message.pooled_embedding().begin(), message.pooled_embedding().end());
    return result;
}
}  // namespace arrakis::news
