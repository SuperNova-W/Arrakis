#include "arrakis/database/postgres.hpp"

#include <libpq-fe.h>

#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace arrakis::database {
namespace {

std::string env_value(const char* name, std::string fallback = {}) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::move(fallback) : std::string(value);
}

void require_result(PGresult* result, ExecStatusType expected = PGRES_COMMAND_OK) {
    if (result == nullptr || PQresultStatus(result) != expected) {
        const std::string message = result == nullptr ? "PostgreSQL returned no result" : PQresultErrorMessage(result);
        throw std::runtime_error("PostgreSQL operation failed: " + message);
    }
}

std::string epoch_ms(std::int64_t value) { return std::to_string(value); }
std::int64_t time_ms(std::string_view value) { return std::stoll(std::string(value)); }
std::chrono::sys_time<std::chrono::milliseconds> parse_timestamp_ms(std::string_view value) {
    return std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{time_ms(value)}};
}

class Result final {
public:
    explicit Result(PGresult* result) : result_(result) {}
    ~Result() { if (result_ != nullptr) PQclear(result_); }
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;
    [[nodiscard]] PGresult* get() const { return result_; }
private:
    PGresult* result_{};
};

struct Connection final {
    explicit Connection(const std::string& connection_string) {
        const char* keywords[] = {
            "dbname",
            "connect_timeout",
            "application_name",
            "keepalives",
            "keepalives_idle",
            nullptr,
        };
        const char* values[] = {
            connection_string.c_str(),
            "5",
            "arrakis-market-api",
            "1",
            "30",
            nullptr,
        };
        handle = PQconnectdbParams(keywords, values, 1);
        if (handle == nullptr || PQstatus(handle) != CONNECTION_OK) {
            const std::string message = handle == nullptr ? "connection allocation failed" : PQerrorMessage(handle);
            if (handle != nullptr) PQfinish(handle);
            throw std::runtime_error("PostgreSQL connection failed: " + message);
        }
    }
    ~Connection() { if (handle != nullptr) PQfinish(handle); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    PGconn* handle{};
};

}  // namespace

struct PostgresPool::Impl {
    explicit Impl(DatabaseConfig value) : config(std::move(value)) {
        if (config.connection_string.empty()) throw std::invalid_argument("DATABASE_URL is required");
        if (config.pool_size == 0) throw std::invalid_argument("database pool size must be positive");
        for (std::size_t i = 0; i < config.pool_size; ++i) connections.push_back(std::make_unique<Connection>(config.connection_string));
    }

    struct Lease {
        Impl& owner;
        std::size_t index;
        PGconn* connection;
        ~Lease() { std::lock_guard lock(owner.mutex); owner.available.push_back(index); owner.condition.notify_one(); }
    };

    [[nodiscard]] Lease acquire() const {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return !available.empty(); });
        const auto index = available.back();
        available.pop_back();
        return Lease{const_cast<Impl&>(*this), index, connections[index]->handle};
    }

    DatabaseConfig config;
    mutable std::mutex mutex;
    mutable std::condition_variable condition;
    std::vector<std::unique_ptr<Connection>> connections;
    mutable std::vector<std::size_t> available;
};

PostgresPool::PostgresPool(DatabaseConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {
    for (std::size_t i = 0; i < impl_->connections.size(); ++i) impl_->available.push_back(i);
}
PostgresPool::~PostgresPool() = default;

bool PostgresPool::healthy() const {
    try {
        const auto lease = impl_->acquire();
        Result result(PQexec(lease.connection, "SELECT 1"));
        return result.get() != nullptr && PQresultStatus(result.get()) == PGRES_TUPLES_OK;
    } catch (...) { return false; }
}

bool PostgresPool::schema_ready() const {
    try {
        const auto lease = impl_->acquire();
        Result result(PQexec(lease.connection,
            "SELECT COUNT(*) FROM pg_class WHERE relname IN ('etf_metadata','etf_bars_1m','etf_bars_5m','etf_bars_daily','news_articles','news_article_entities','news_nlp_features','etf_daily_news_features','model_registry')"));
        return result.get() != nullptr && PQresultStatus(result.get()) == PGRES_TUPLES_OK &&
            std::string(PQgetvalue(result.get(), 0, 0)) == "9";
    } catch (...) { return false; }
}

void PostgresPool::persist_bars(const std::vector<bar_aggregator::MarketBar>& one_minute,
                                const std::vector<bar_aggregator::MarketBar>& five_minute) {
    if (one_minute.empty() && five_minute.empty()) return;
    const auto lease = impl_->acquire();
    Result begin(PQexec(lease.connection, "BEGIN")); require_result(begin.get());
    try {
        const auto insert = [&](const bar_aggregator::MarketBar& bar) {
            const std::string table = bar.interval == "5m" ? "5m" : "1m";
            const std::string start = epoch_ms(bar.bar_start.time_since_epoch().count());
            const std::string end = epoch_ms(bar.bar_end.time_since_epoch().count());
            const std::string first = epoch_ms(bar.first_trade_time.time_since_epoch().count());
            const std::string last = epoch_ms(bar.last_trade_time.time_since_epoch().count());
            const std::string open = std::to_string(bar.open), high = std::to_string(bar.high), low = std::to_string(bar.low), close = std::to_string(bar.close), volume = std::to_string(bar.volume), trades = std::to_string(bar.trade_count);
            const char* values[] = {bar.event_id.c_str(), bar.symbol.c_str(), start.c_str(), end.c_str(), open.c_str(), high.c_str(), low.c_str(), close.c_str(), volume.c_str(), trades.c_str(), first.c_str(), last.c_str(), "finnhub"};
            const std::string statement = "INSERT INTO etf_bars_" + table + " (bar_id,symbol,bar_start,bar_end,open,high,low,close,volume,trade_count,first_trade_timestamp,last_trade_timestamp,source) VALUES ($1,$2,to_timestamp($3::double precision/1000.0),to_timestamp($4::double precision/1000.0),$5,$6,$7,$8,$9,$10,to_timestamp($11::double precision/1000.0),to_timestamp($12::double precision/1000.0),$13) ON CONFLICT DO NOTHING";
            Result result(PQexecParams(lease.connection, statement.c_str(), 13, nullptr, values, nullptr, nullptr, 0));
            require_result(result.get());
        };
        for (const auto& bar : one_minute) insert(bar);
        for (const auto& bar : five_minute) insert(bar);
        Result commit(PQexec(lease.connection, "COMMIT")); require_result(commit.get());
    } catch (...) {
        Result rollback(PQexec(lease.connection, "ROLLBACK"));
        static_cast<void>(rollback);
        throw;
    }
}

std::vector<EtfMetadata> PostgresPool::list_etfs() const {
    const auto lease = impl_->acquire();
    Result result(PQexec(lease.connection, "SELECT symbol,name,category,active FROM etf_metadata WHERE active ORDER BY category,symbol"));
    require_result(result.get(), PGRES_TUPLES_OK);
    std::vector<EtfMetadata> output;
    for (int row = 0; row < PQntuples(result.get()); ++row) output.push_back({PQgetvalue(result.get(), row, 0), PQgetvalue(result.get(), row, 1), PQgetvalue(result.get(), row, 2), std::string(PQgetvalue(result.get(), row, 3)) == "t"});
    return output;
}

std::optional<bar_aggregator::MarketBar> PostgresPool::latest_bar(std::string_view symbol, std::string_view interval) const {
    const auto rows = bars(symbol, interval, std::nullopt, std::nullopt, 1);
    return rows.empty() ? std::nullopt : std::optional<bar_aggregator::MarketBar>(rows.front());
}

std::vector<bar_aggregator::MarketBar> PostgresPool::bars(std::string_view symbol, std::string_view interval,
    std::optional<std::chrono::sys_time<std::chrono::milliseconds>> from,
    std::optional<std::chrono::sys_time<std::chrono::milliseconds>> to, std::size_t limit) const {
    if (interval != "1m" && interval != "5m") throw std::invalid_argument("interval must be 1m or 5m");
    const auto lease = impl_->acquire();
    const std::string table = interval == "5m" ? "etf_bars_5m" : "etf_bars_1m";
    const std::string query =
        "SELECT bar_id,symbol,EXTRACT(EPOCH FROM bar_start)*1000,"
        "EXTRACT(EPOCH FROM bar_end)*1000,open,high,low,close,volume,trade_count,"
        "EXTRACT(EPOCH FROM first_trade_timestamp)*1000,"
        "EXTRACT(EPOCH FROM last_trade_timestamp)*1000 FROM " +
        table +
        " WHERE symbol=$1 AND ($2='' OR bar_start >= "
        "to_timestamp($2::double precision/1000.0)) AND ($3='' OR bar_end <= "
        "to_timestamp($3::double precision/1000.0)) ORDER BY bar_start DESC LIMIT $4";
    const std::string from_value = from ? std::to_string(from->time_since_epoch().count()) : "";
    const std::string to_value = to ? std::to_string(to->time_since_epoch().count()) : "";
    const std::string limit_value = std::to_string(limit);
    const std::string symbol_value(symbol);
    const char* values[] = {symbol_value.c_str(), from_value.c_str(), to_value.c_str(), limit_value.c_str()};
    Result result(PQexecParams(lease.connection, query.c_str(), 4, nullptr, values, nullptr, nullptr, 0));
    require_result(result.get(), PGRES_TUPLES_OK);
    std::vector<bar_aggregator::MarketBar> output;
    for (int row = PQntuples(result.get()) - 1; row >= 0; --row) {
        const auto ms = [&](int column) { return time_ms(PQgetvalue(result.get(), row, column)); };
        output.push_back({PQgetvalue(result.get(), row, 0), PQgetvalue(result.get(), row, 1), std::string(interval),
            std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{ms(2)}},
            std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{ms(3)}},
            std::stod(PQgetvalue(result.get(), row, 4)), std::stod(PQgetvalue(result.get(), row, 5)), std::stod(PQgetvalue(result.get(), row, 6)), std::stod(PQgetvalue(result.get(), row, 7)), std::stod(PQgetvalue(result.get(), row, 8)), static_cast<std::uint64_t>(std::stoull(PQgetvalue(result.get(), row, 9))),
            std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{ms(10)}}, std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{ms(11)}}, true});
    }
    return output;
}

std::vector<DailyMarketBar> PostgresPool::daily_market_bars(
    std::string_view symbol, std::int64_t cutoff_unix_ms, std::size_t lookback_days) const {
    // Completed sessions come from the true end-of-day table so that the live
    // features are on the same scale as the training dataset.  Only the
    // still-open session is reconstructed from the intraday stream.  Both
    // branches respect the point-in-time cutoff: a daily bar is admitted only
    // once its 16:00 America/New_York close is at or before the cutoff, and
    // intraday bars only when bar_end is at or before the cutoff.
    const auto lease = impl_->acquire();
    const std::string symbol_value(symbol);
    const std::string cutoff = std::to_string(cutoff_unix_ms);
    const std::string lookback = std::to_string(lookback_days);
    const char* values[] = {symbol_value.c_str(), cutoff.c_str(), lookback.c_str()};

    constexpr std::string_view completed_query =
        "SELECT trading_date::text, close, volume FROM etf_bars_daily"
        " WHERE symbol=$1"
        "   AND trading_date <= (to_timestamp($2::double precision/1000.0)"
        "       AT TIME ZONE 'America/New_York')::date"
        "   AND trading_date >= (to_timestamp($2::double precision/1000.0)"
        "       AT TIME ZONE 'America/New_York')::date - $3::integer"
        " ORDER BY trading_date";
    Result completed_result(
        PQexecParams(lease.connection, completed_query.data(), 3, nullptr, values, nullptr, nullptr, 0));
    require_result(completed_result.get(), PGRES_TUPLES_OK);
    std::vector<DailyMarketBar> completed;
    completed.reserve(static_cast<std::size_t>(PQntuples(completed_result.get())));
    for (int row = 0; row < PQntuples(completed_result.get()); ++row) {
        completed.push_back({PQgetvalue(completed_result.get(), row, 0),
                             std::stod(PQgetvalue(completed_result.get(), row, 1)),
                             std::stod(PQgetvalue(completed_result.get(), row, 2))});
    }
    completed = filter_completed_sessions(std::move(completed), cutoff_unix_ms);

    constexpr std::string_view in_progress_query =
        "SELECT (bar_end AT TIME ZONE 'America/New_York')::date::text AS trading_date,"
        "       (array_agg(close ORDER BY bar_end DESC))[1] AS close,"
        "       SUM(volume) AS volume"
        " FROM etf_bars_5m"
        " WHERE symbol=$1"
        "   AND bar_end <= to_timestamp($2::double precision/1000.0)"
        "   AND (bar_end AT TIME ZONE 'America/New_York')::date ="
        "       (to_timestamp($2::double precision/1000.0) AT TIME ZONE 'America/New_York')::date"
        " GROUP BY 1";
    Result in_progress_result(
        PQexecParams(lease.connection, in_progress_query.data(), 2, nullptr, values, nullptr, nullptr, 0));
    require_result(in_progress_result.get(), PGRES_TUPLES_OK);
    std::vector<DailyMarketBar> in_progress;
    in_progress.reserve(static_cast<std::size_t>(PQntuples(in_progress_result.get())));
    for (int row = 0; row < PQntuples(in_progress_result.get()); ++row) {
        in_progress.push_back({PQgetvalue(in_progress_result.get(), row, 0),
                               std::stod(PQgetvalue(in_progress_result.get(), row, 1)),
                               std::stod(PQgetvalue(in_progress_result.get(), row, 2))});
    }

    return merge_daily_and_intraday(std::move(completed), in_progress);
}

std::size_t PostgresPool::upsert_daily_bars(const std::vector<DailyBarRecord>& bars, std::string_view source) {
    if (bars.empty()) return 0;
    const auto lease = impl_->acquire();
    const std::string source_value(source);
    Result begin(PQexec(lease.connection, "BEGIN"));
    require_result(begin.get());
    try {
        for (const auto& bar : bars) {
            const std::string open = std::to_string(bar.open), high = std::to_string(bar.high),
                              low = std::to_string(bar.low), close = std::to_string(bar.close),
                              volume = std::to_string(bar.volume);
            const char* values[] = {bar.symbol.c_str(), bar.trading_date.c_str(), open.c_str(), high.c_str(),
                                    low.c_str(),        close.c_str(),            volume.c_str(),
                                    source_value.c_str()};
            Result result(PQexecParams(lease.connection,
                "INSERT INTO etf_bars_daily(symbol,trading_date,open,high,low,close,volume,source)"
                " VALUES($1,$2::date,$3,$4,$5,$6,$7,$8)"
                " ON CONFLICT(symbol,trading_date) DO UPDATE SET"
                " open=EXCLUDED.open,high=EXCLUDED.high,low=EXCLUDED.low,close=EXCLUDED.close,"
                " volume=EXCLUDED.volume,source=EXCLUDED.source,inserted_at=NOW()",
                8, nullptr, values, nullptr, nullptr, 0));
            require_result(result.get());
        }
        Result commit(PQexec(lease.connection, "COMMIT"));
        require_result(commit.get());
    } catch (...) {
        Result rollback(PQexec(lease.connection, "ROLLBACK"));
        static_cast<void>(rollback);
        throw;
    }
    return bars.size();
}

void PostgresPool::persist_news_article(const NewsArticle& article, std::string_view normalized_content_hash,
                                        std::string_view provenance_json) {
    const auto lease = impl_->acquire();
    const std::string published = std::to_string(article.published_at.time_since_epoch().count());
    const std::string retrieved = std::to_string(article.retrieved_at.time_since_epoch().count());
    const std::string novelty = std::to_string(article.novelty_score);
    const std::string hash(normalized_content_hash), provenance(provenance_json);
    const char* values[] = {article.article_id.c_str(), article.canonical_url.c_str(), hash.c_str(), article.source_id.c_str(), article.headline.c_str(), article.body.c_str(), published.c_str(), retrieved.c_str(), novelty.c_str(), provenance.c_str()};
    Result result(PQexecParams(lease.connection,
        "INSERT INTO news_articles(article_id,canonical_url,normalized_content_hash,source_id,headline,body,published_at,retrieved_at,novelty_score,provenance) VALUES($1,$2,$3,$4,$5,$6,to_timestamp($7::double precision/1000),to_timestamp($8::double precision/1000),$9,$10::jsonb) ON CONFLICT DO NOTHING",
        10, nullptr, values, nullptr, nullptr, 0));
    require_result(result.get());
}

void PostgresPool::persist_news_entities(std::string_view article_id, const std::vector<std::string>& entities) {
    const auto lease = impl_->acquire();
    const std::string article(article_id);
    for (const auto& entity : entities) {
        const std::string type = entity.starts_with("XLK") || entity.size() <= 5 ? "etf" : entity.starts_with("macro:") ? "macro" : entity.starts_with("sector:") ? "sector" : "company";
        const std::string entity_id = entity.starts_with("macro:") || entity.starts_with("sector:") ? entity.substr(entity.find(':') + 1) : entity;
        const char* values[] = {article.c_str(), type.c_str(), entity_id.c_str()};
        Result result(PQexecParams(lease.connection, "INSERT INTO news_article_entities(article_id,entity_type,entity_id) VALUES($1,$2,$3) ON CONFLICT DO NOTHING", 3, nullptr, values, nullptr, nullptr, 0));
        require_result(result.get());
    }
}

void PostgresPool::persist_news_features(std::string_view article_id, std::string_view model_version,
                                         std::string_view tokenizer_version, double positive_probability,
                                         double neutral_probability, double negative_probability, double sentiment_score,
                                         std::string_view embedding_json, std::string_view feature_schema_hash,
                                         double inference_latency_ms) {
    const auto lease = impl_->acquire();
    const std::string positive = std::to_string(positive_probability), neutral = std::to_string(neutral_probability), negative = std::to_string(negative_probability), sentiment = std::to_string(sentiment_score), latency = std::to_string(inference_latency_ms);
    const std::string article(article_id), model(model_version), tokenizer(tokenizer_version), embedding(embedding_json), schema(feature_schema_hash);
    const char* values[] = {article.c_str(), model.c_str(), tokenizer.c_str(), positive.c_str(), neutral.c_str(), negative.c_str(), sentiment.c_str(), embedding.c_str(), schema.c_str(), latency.c_str()};
    Result result(PQexecParams(lease.connection, "INSERT INTO news_nlp_features(article_id,model_version,tokenizer_version,positive_probability,neutral_probability,negative_probability,sentiment_score,pooled_embedding,feature_schema_hash,inference_latency_ms) VALUES($1,$2,$3,$4,$5,$6,$7,$8::jsonb,$9,$10) ON CONFLICT(article_id) DO UPDATE SET model_version=EXCLUDED.model_version,tokenizer_version=EXCLUDED.tokenizer_version,positive_probability=EXCLUDED.positive_probability,neutral_probability=EXCLUDED.neutral_probability,negative_probability=EXCLUDED.negative_probability,sentiment_score=EXCLUDED.sentiment_score,pooled_embedding=EXCLUDED.pooled_embedding,feature_schema_hash=EXCLUDED.feature_schema_hash,inference_latency_ms=EXCLUDED.inference_latency_ms", 10, nullptr, values, nullptr, nullptr, 0));
    require_result(result.get());
}

void PostgresPool::persist_daily_news_features(std::string_view symbol, std::string_view trading_date,
                                               std::string_view cutoff_timestamp, std::string_view latest_article_timestamp,
                                               std::string_view feature_schema_hash, std::string_view features_json,
                                               int article_count, std::string_view coverage_status,
                                               std::string_view missing_warnings_json) {
    const auto lease = impl_->acquire();
    const std::string symbol_value(symbol), date_value(trading_date), cutoff(cutoff_timestamp), latest(latest_article_timestamp), schema(feature_schema_hash), features(features_json), count = std::to_string(article_count), status(coverage_status), warnings(missing_warnings_json);
    const char* values[] = {symbol_value.c_str(), date_value.c_str(), cutoff.c_str(), latest.c_str(), schema.c_str(), features.c_str(), count.c_str(), status.c_str(), warnings.c_str()};
    Result result(PQexecParams(lease.connection, "INSERT INTO etf_daily_news_features(symbol,trading_date,cutoff_timestamp,latest_eligible_article_at,feature_schema_hash,features,article_count,coverage_status,missing_source_warnings) VALUES($1,$2::date,$3::timestamptz,NULLIF($4,'')::timestamptz,$5,$6::jsonb,$7,$8,$9::jsonb) ON CONFLICT(symbol,trading_date) DO UPDATE SET cutoff_timestamp=EXCLUDED.cutoff_timestamp,latest_eligible_article_at=EXCLUDED.latest_eligible_article_at,feature_schema_hash=EXCLUDED.feature_schema_hash,features=EXCLUDED.features,article_count=EXCLUDED.article_count,coverage_status=EXCLUDED.coverage_status,missing_source_warnings=EXCLUDED.missing_source_warnings", 9, nullptr, values, nullptr, nullptr, 0));
    require_result(result.get());
}

NewsFeatureSnapshot PostgresPool::news_snapshot(std::string_view symbol, std::string_view trading_date, std::size_t article_limit) const {
    const auto lease = impl_->acquire();
    NewsFeatureSnapshot output;
    output.symbol = symbol; output.trading_date = trading_date;
    const std::string symbol_value(symbol), date_value(trading_date), limit = std::to_string(article_limit);
    const char* daily_values[] = {symbol_value.c_str(), date_value.c_str()};
    Result daily(PQexecParams(lease.connection, "SELECT cutoff_timestamp::text,COALESCE(latest_eligible_article_at::text,''),feature_schema_hash,features::text,coverage_status,missing_source_warnings::text FROM etf_daily_news_features WHERE symbol=$1 AND trading_date=$2::date", 2, nullptr, daily_values, nullptr, nullptr, 0));
    if (daily.get() != nullptr && PQresultStatus(daily.get()) == PGRES_TUPLES_OK && PQntuples(daily.get()) > 0) {
        output.cutoff_timestamp = PQgetvalue(daily.get(), 0, 0); output.latest_eligible_article_at = PQgetvalue(daily.get(), 0, 1); output.feature_schema_hash = PQgetvalue(daily.get(), 0, 2); output.features_json = PQgetvalue(daily.get(), 0, 3); output.coverage_status = PQgetvalue(daily.get(), 0, 4); output.missing_source_warnings_json = PQgetvalue(daily.get(), 0, 5);
    }
    const char* article_values[] = {symbol_value.c_str(), date_value.c_str(), limit.c_str()};
    Result articles(PQexecParams(lease.connection, "SELECT DISTINCT a.article_id,a.canonical_url,a.source_id,a.headline,a.body,EXTRACT(EPOCH FROM a.published_at)*1000,EXTRACT(EPOCH FROM a.retrieved_at)*1000,a.novelty_score,COALESCE(n.positive_probability,0),COALESCE(n.neutral_probability,0),COALESCE(n.negative_probability,0),COALESCE(n.sentiment_score,0) FROM news_articles a JOIN news_article_entities e ON e.article_id=a.article_id LEFT JOIN news_nlp_features n ON n.article_id=a.article_id WHERE e.entity_type='etf' AND e.entity_id=$1 AND a.published_at <= (SELECT cutoff_timestamp FROM etf_daily_news_features WHERE symbol=$1 AND trading_date=$2::date) ORDER BY a.published_at DESC LIMIT $3", 3, nullptr, article_values, nullptr, nullptr, 0));
    if (articles.get() != nullptr && PQresultStatus(articles.get()) == PGRES_TUPLES_OK) {
        for (int row = 0; row < PQntuples(articles.get()); ++row) {
            NewsArticle article{PQgetvalue(articles.get(), row, 0), PQgetvalue(articles.get(), row, 1), PQgetvalue(articles.get(), row, 2), PQgetvalue(articles.get(), row, 3), PQgetvalue(articles.get(), row, 4), parse_timestamp_ms(PQgetvalue(articles.get(), row, 5)), parse_timestamp_ms(PQgetvalue(articles.get(), row, 6)), std::stod(PQgetvalue(articles.get(), row, 7)), std::stod(PQgetvalue(articles.get(), row, 8)), std::stod(PQgetvalue(articles.get(), row, 9)), std::stod(PQgetvalue(articles.get(), row, 10)), std::stod(PQgetvalue(articles.get(), row, 11)), {}};
            output.articles.push_back(std::move(article));
        }
    }
    return output;
}

DatabaseConfig database_config_from_environment() {
    DatabaseConfig config;
    config.connection_string = env_value("SUPABASE_DB_URL", env_value("DATABASE_URL"));
    if (config.connection_string.empty()) {
        config.connection_string =
            "host=" + env_value("POSTGRES_HOST", "localhost") +
            " port=" + env_value("POSTGRES_PORT", "5432") +
            " dbname=" + env_value("POSTGRES_DB", "market_data") +
            " user=" + env_value("POSTGRES_USER", "market_app") +
            " password=" + env_value("POSTGRES_PASSWORD") +
            " sslmode=" + env_value("POSTGRES_SSLMODE", "prefer");
    }
    if (const auto value = env_value("POSTGRES_POOL_SIZE"); !value.empty()) config.pool_size = static_cast<std::size_t>(std::stoul(value));
    return config;
}

}  // namespace arrakis::database
