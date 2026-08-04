#include "arrakis/database/postgres.hpp"
#include "arrakis/news/aggregation.hpp"
#include "arrakis/news/finbert.hpp"
#include "arrakis/serialization/news_serialization.hpp"
#include "arrakis/streaming/kafka.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::string env(const char* name, std::string fallback = {}) { const char* value = std::getenv(name); return value == nullptr ? std::move(fallback) : std::string(value); }
std::int64_t env_int(const char* name, std::int64_t fallback) { const auto value = env(name); return value.empty() ? fallback : std::stoll(value); }
bool contains(const std::vector<std::string>& values, std::string_view target) { for (const auto& value : values) if (value == target) return true; return false; }
std::string embedding_json(const std::vector<double>& values) { boost::json::array output; for (const auto value : values) output.push_back(value); return boost::json::serialize(output); }
std::string iso_from_ms(std::int64_t value) {
    const auto time = std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{value}});
    const std::time_t raw_time = std::chrono::system_clock::to_time_t(time);
    std::tm utc_time{};
#if defined(_WIN32)
    gmtime_s(&utc_time, &raw_time);
#else
    gmtime_r(&raw_time, &utc_time);
#endif
    std::ostringstream output;
    output << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}
}

int main() {
    try {
        arrakis::database::PostgresPool database(arrakis::database::database_config_from_environment());
        arrakis::news::FinbertSession finbert(env("ARRAKIS_FINBERT_ONNX_PATH"), env("ARRAKIS_FINBERT_VOCAB_PATH"), env("ARRAKIS_FINBERT_VERSION", "finbert-v1"), env("ARRAKIS_FINBERT_TOKENIZER_VERSION", "finbert-tokenizer-v1"), static_cast<std::size_t>(std::stoul(env("ARRAKIS_FINBERT_MAX_TOKENS", "128"))));
        arrakis::streaming::KafkaConsumer consumer(env("KAFKA_BOOTSTRAP_SERVERS", "localhost:9092"), env("NEWS_CONSUMER_GROUP", "news-enricher-v1"), env("NEWS_RAW_TOPIC", "news.raw.articles"));
        arrakis::streaming::KafkaProducer producer(env("KAFKA_BOOTSTRAP_SERVERS", "localhost:9092"), "news-enricher-v1");
        const auto cutoff = env_int("NEWS_PREDICTION_CUTOFF_UNIX_MS", 0);
        const auto trading_date = env("NEWS_TRADING_DATE");
        const auto cutoff_iso = env("NEWS_PREDICTION_CUTOFF_ISO");
        if (cutoff <= 0 || trading_date.empty() || cutoff_iso.empty()) throw std::invalid_argument("NEWS_PREDICTION_CUTOFF_UNIX_MS, NEWS_PREDICTION_CUTOFF_ISO, and NEWS_TRADING_DATE are required");
        std::vector<arrakis::news::EnrichedArticle> aggregate;
        for (;;) {
            const auto record = consumer.poll(std::chrono::milliseconds{1000});
            if (!record) continue;
            try {
                const auto article = arrakis::news::deserialize_article(record->payload);
                if (article.published_at_unix_ms > cutoff || !contains(article.entity_ids, "XLK")) { consumer.commit(*record); continue; }
                const auto outputs = finbert.infer({article.headline + "\n" + article.body});
                if (outputs.size() != 1) throw std::runtime_error("FinBERT returned an unexpected batch size");
                const auto& output = outputs.front();
                arrakis::database::NewsArticle database_article{article.article_id, article.canonical_url, article.source_id, article.headline, article.body, std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{article.published_at_unix_ms}}, std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{article.retrieved_at_unix_ms}}, 1.0, 0.0, 0.0, 0.0, 0.0, article.entity_ids};
                database.persist_news_article(database_article, article.normalized_content_hash, "{\"provider\":\"approved-source\"}");
                database.persist_news_entities(article.article_id, article.entity_ids);
                database.persist_news_features(article.article_id, finbert.model_version(), finbert.tokenizer_version(), output.positive_probability, output.neutral_probability, output.negative_probability, output.sentiment_score, embedding_json(output.pooled_embedding), "xlk-news-features-v1", 0.0);
                aggregate.push_back({article, {article.article_id, finbert.model_version(), finbert.tokenizer_version(), output.positive_probability, output.neutral_probability, output.negative_probability, output.sentiment_score, output.pooled_embedding, 1.0, cutoff}, 1.0, contains(article.entity_ids, "company:MSFT") || contains(article.entity_ids, "company:NVDA"), contains(article.entity_ids, "macro:rates") || contains(article.entity_ids, "macro:inflation"), contains(article.entity_ids, "sector:technology") || contains(article.entity_ids, "sector:semiconductors")});
                const auto daily = arrakis::news::aggregate_daily(trading_date, cutoff, aggregate);
                const auto latest_iso = daily.latest_article_unix_ms > 0 ? iso_from_ms(daily.latest_article_unix_ms) : std::string{};
                database.persist_daily_news_features("XLK", trading_date, cutoff_iso, latest_iso, daily.feature_schema_hash, daily.to_json(), static_cast<int>(aggregate.size()), daily.coverage_status, "[]");
                const auto enriched = arrakis::news::serialize_enriched_feature({article.article_id, finbert.model_version(), finbert.tokenizer_version(), output.positive_probability, output.neutral_probability, output.negative_probability, output.sentiment_score, output.pooled_embedding, 1.0, cutoff});
                producer.publish(env("NEWS_ENRICHED_TOPIC", "news.enriched.features"), "XLK", enriched); producer.poll_events(std::chrono::milliseconds{0}); consumer.commit(*record);
            } catch (const std::exception& error) { std::cerr << "{\"service\":\"news-enricher\",\"error\":\"" << error.what() << "\"}\n"; }
        }
    } catch (const std::exception& error) { std::cerr << "{\"service\":\"news-enricher\",\"fatal\":\"" << error.what() << "\"}\n"; return EXIT_FAILURE; }
}
