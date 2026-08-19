#include "arrakis/database/postgres.hpp"
#include "arrakis/news/aggregation.hpp"
#include "arrakis/news/finbert.hpp"
#include "arrakis/news/market_features.hpp"
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
// Train/serve parity: build_xlk_combined_dataset marks every training article
// entity_weight=1.0, holding_related=true, sector_related=true, macro_related=false,
// because each one is by construction a point-in-time XLK constituent article.
// The live path must classify the same way, so an article counts as holding-related
// exactly when news-ingestion tagged it with a resolved constituent.
bool has_company_entity(const std::vector<std::string>& values) {
    for (const auto& value : values) if (value.starts_with("company:")) return true;
    return false;
}
std::string embedding_json(const std::vector<double>& values) { boost::json::array output; for (const auto value : values) output.push_back(value); return boost::json::serialize(output); }
struct PredictionWindow final {
    std::int64_t cutoff_unix_ms{};
    std::string trading_date;
    std::string cutoff_iso;
};

PredictionWindow current_window() {
    const auto now = std::chrono::system_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    // UTC is ahead of New York during the entire market session. The six-hour
    // shift keeps the date aligned with the US trading date across DST.
    const auto reference_time = now - std::chrono::hours{6};
    auto raw_time = std::chrono::system_clock::to_time_t(reference_time);
    std::tm utc{};
    gmtime_r(&raw_time, &utc);
    while (utc.tm_wday == 0 || utc.tm_wday == 6) {
        raw_time -= 24 * 60 * 60;
        gmtime_r(&raw_time, &utc);
    }
    std::ostringstream date;
    date << std::put_time(&utc, "%Y-%m-%d");
    std::ostringstream cutoff;
    cutoff << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return {now_ms, date.str(), cutoff.str()};
}

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
        // Inclusive lower bound on publication time, matching the one-day
        // grouping the batch dataset builder uses. Without it the aggregate
        // spans the whole NEWS_POLL_LOOKBACK_DAYS republication window and
        // article_count is inflated several-fold against the trained schema.
        // 0 preserves the previous unbounded behaviour.
        const auto window_start = env_int("NEWS_PREDICTION_WINDOW_START_UNIX_MS", 0);
        const auto configured_cutoff = env_int("NEWS_PREDICTION_CUTOFF_UNIX_MS", 0);
        const auto configured_date = env("NEWS_TRADING_DATE");
        const auto configured_cutoff_iso = env("NEWS_PREDICTION_CUTOFF_ISO");
        const bool fixed_window = configured_cutoff > 0 && !configured_date.empty() && !configured_cutoff_iso.empty();
        std::vector<arrakis::news::EnrichedArticle> aggregate;
        std::string aggregate_date = configured_date;
        for (;;) {
            const auto record = consumer.poll(std::chrono::milliseconds{1000});
            if (!record) continue;
            try {
                const auto window = fixed_window
                    ? PredictionWindow{configured_cutoff, configured_date, configured_cutoff_iso}
                    : current_window();
                if (aggregate_date != window.trading_date) {
                    aggregate.clear();
                    aggregate_date = window.trading_date;
                }
                const auto article = arrakis::news::deserialize_article(record->payload);
                if (article.published_at_unix_ms > window.cutoff_unix_ms || article.published_at_unix_ms < window_start || !contains(article.entity_ids, "XLK")) { consumer.commit(*record); continue; }
                const auto outputs = finbert.infer({article.headline + "\n" + article.body});
                if (outputs.size() != 1) throw std::runtime_error("FinBERT returned an unexpected batch size");
                const auto& output = outputs.front();
                arrakis::database::NewsArticle database_article{article.article_id, article.canonical_url, article.source_id, article.headline, article.body, std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{article.published_at_unix_ms}}, std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{article.retrieved_at_unix_ms}}, 1.0, 0.0, 0.0, 0.0, 0.0, article.entity_ids};
                database.persist_news_article(database_article, article.normalized_content_hash, "{\"provider\":\"approved-source\"}");
                database.persist_news_entities(article.article_id, article.entity_ids);
                database.persist_news_features(article.article_id, finbert.model_version(), finbert.tokenizer_version(), output.positive_probability, output.neutral_probability, output.negative_probability, output.sentiment_score, embedding_json(output.pooled_embedding), "xlk-news-features-v1", 0.0);
                aggregate.push_back({article, {article.article_id, finbert.model_version(), finbert.tokenizer_version(), output.positive_probability, output.neutral_probability, output.negative_probability, output.sentiment_score, output.pooled_embedding, 1.0, window.cutoff_unix_ms}, 1.0, has_company_entity(article.entity_ids), false, true});
                const auto daily = arrakis::news::aggregate_daily(window.trading_date, window.cutoff_unix_ms, aggregate, window_start);
                const auto xlk_bars = database.daily_market_bars("XLK", window.cutoff_unix_ms);
                const auto spy_bars = database.daily_market_bars("SPY", window.cutoff_unix_ms);
                std::vector<arrakis::news::MarketDay> xlk_days;
                std::vector<arrakis::news::MarketDay> spy_days;
                xlk_days.reserve(xlk_bars.size());
                spy_days.reserve(spy_bars.size());
                for (const auto& bar : xlk_bars) xlk_days.push_back({bar.trading_date, bar.close, bar.volume});
                for (const auto& bar : spy_bars) spy_days.push_back({bar.trading_date, bar.close, bar.volume});
                const auto market_values = arrakis::news::market_feature_vector(xlk_days, spy_days, window.trading_date);
                if (!market_values) throw std::runtime_error{"Persisted market history is insufficient for " + window.trading_date};
                const auto latest_iso = daily.latest_article_unix_ms > 0 ? iso_from_ms(daily.latest_article_unix_ms) : std::string{};
                database.persist_daily_news_features("XLK", window.trading_date, window.cutoff_iso, latest_iso, std::string{arrakis::news::kCombinedFeatureSchemaHash}, daily.to_combined_json(*market_values), static_cast<int>(aggregate.size()), daily.coverage_status, "[]");
                const auto enriched = arrakis::news::serialize_enriched_feature({article.article_id, finbert.model_version(), finbert.tokenizer_version(), output.positive_probability, output.neutral_probability, output.negative_probability, output.sentiment_score, output.pooled_embedding, 1.0, window.cutoff_unix_ms});
                producer.publish(env("NEWS_ENRICHED_TOPIC", "news.enriched.features"), "XLK", enriched); producer.poll_events(std::chrono::milliseconds{0}); consumer.commit(*record);
            } catch (const std::exception& error) { std::cerr << "{\"service\":\"news-enricher\",\"error\":\"" << error.what() << "\"}\n"; }
        }
    } catch (const std::exception& error) { std::cerr << "{\"service\":\"news-enricher\",\"fatal\":\"" << error.what() << "\"}\n"; return EXIT_FAILURE; }
}
