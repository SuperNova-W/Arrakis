#pragma once

#include "arrakis/bar_aggregator/aggregation.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arrakis::database {

struct DatabaseConfig {
    std::string connection_string;
    std::size_t pool_size{4};
    int connect_timeout_seconds{5};
};

struct EtfMetadata {
    std::string symbol;
    std::string name;
    std::string category;
    bool active{true};
};

struct NewsArticle final {
    std::string article_id;
    std::string canonical_url;
    std::string source_id;
    std::string headline;
    std::string body;
    std::chrono::sys_time<std::chrono::milliseconds> published_at{};
    std::chrono::sys_time<std::chrono::milliseconds> retrieved_at{};
    double novelty_score{1.0};
    double positive_probability{};
    double neutral_probability{};
    double negative_probability{};
    double sentiment_score{};
    std::vector<std::string> entities;
};

struct NewsFeatureSnapshot final {
    std::string symbol;
    std::string trading_date;
    std::string cutoff_timestamp;
    std::string latest_eligible_article_at;
    std::string coverage_status;
    std::string feature_schema_hash;
    std::string features_json;
    std::string missing_source_warnings_json;
    std::vector<NewsArticle> articles;
};

struct DailyMarketBar final {
    std::string trading_date;
    double close{};
    double volume{};
};

class PostgresPool final {
public:
    explicit PostgresPool(DatabaseConfig config);
    ~PostgresPool();
    PostgresPool(const PostgresPool&) = delete;
    PostgresPool& operator=(const PostgresPool&) = delete;

    [[nodiscard]] bool healthy() const;
    [[nodiscard]] bool schema_ready() const;
    void persist_bars(const std::vector<bar_aggregator::MarketBar>& one_minute,
                      const std::vector<bar_aggregator::MarketBar>& five_minute);
    [[nodiscard]] std::vector<EtfMetadata> list_etfs() const;
    [[nodiscard]] std::optional<bar_aggregator::MarketBar> latest_bar(
        std::string_view symbol, std::string_view interval) const;
    [[nodiscard]] std::vector<bar_aggregator::MarketBar> bars(
        std::string_view symbol,
        std::string_view interval,
        std::optional<std::chrono::sys_time<std::chrono::milliseconds>> from,
        std::optional<std::chrono::sys_time<std::chrono::milliseconds>> to,
        std::size_t limit) const;
    [[nodiscard]] std::vector<DailyMarketBar> daily_market_bars(
        std::string_view symbol, std::int64_t cutoff_unix_ms, std::size_t lookback_days = 45) const;
    void persist_news_article(const NewsArticle& article, std::string_view normalized_content_hash,
                              std::string_view provenance_json);
    void persist_news_entities(std::string_view article_id, const std::vector<std::string>& entities);
    void persist_news_features(std::string_view article_id, std::string_view model_version,
                               std::string_view tokenizer_version, double positive_probability,
                               double neutral_probability, double negative_probability, double sentiment_score,
                               std::string_view embedding_json, std::string_view feature_schema_hash,
                               double inference_latency_ms);
    void persist_daily_news_features(std::string_view symbol, std::string_view trading_date,
                                     std::string_view cutoff_timestamp, std::string_view latest_article_timestamp,
                                     std::string_view feature_schema_hash, std::string_view features_json,
                                     int article_count, std::string_view coverage_status,
                                     std::string_view missing_warnings_json);
    [[nodiscard]] NewsFeatureSnapshot news_snapshot(std::string_view symbol, std::string_view trading_date,
                                                    std::size_t article_limit = 20) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] DatabaseConfig database_config_from_environment();

}  // namespace arrakis::database
