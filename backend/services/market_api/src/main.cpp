#include "arrakis/database/postgres.hpp"
#include "arrakis/market_api/live_market.hpp"
#include "arrakis/news/feature_schema.hpp"
#include "arrakis/news/finbert.hpp"
#include "arrakis/serialization/serialization.hpp"
#include "arrakis/streaming/kafka.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <xgboost/c_api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <atomic>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <thread>
#include <unordered_map>

namespace net = boost::asio;
namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;
using tcp = net::ip::tcp;
using arrakis::bar_aggregator::MarketBar;
using arrakis::market_api::LiveMarketStore;
using arrakis::market_api::LiveUpdate;

namespace {

std::atomic<std::uint64_t> request_sequence{0};

struct RuntimeState {
    std::atomic<bool> kafka_consumer_running{false};
    std::atomic<std::uint64_t> kafka_records{0};
    std::atomic<std::uint64_t> kafka_rejected_records{0};
    std::atomic<std::uint64_t> api_requests{0};
    std::atomic<std::uint64_t> connection_errors{0};
};

std::string env(const char* name, std::string fallback = {}) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::move(fallback) : std::string(value);
}

bool env_true(const char* name) {
    const auto value = env(name, "false");
    return value == "1" || value == "true" || value == "TRUE" || value == "yes";
}

class NewsXGBoostModel final {
public:
    NewsXGBoostModel() {
        if (!env_true("ARRAKIS_XLK_NEWS_MODEL_VALIDATED")) {
            throw std::runtime_error{
                "No validated XLK model is enabled; walk-forward promotion evidence is required"
            };
        }
        const auto path = env("ARRAKIS_XLK_NEWS_MODEL_PATH", "artifacts/xlk_news_xgboost.json");
        if (!std::filesystem::exists(path)) throw std::runtime_error("Local XGBoost artifact is missing: " + path);
        if (XGBoosterCreate(nullptr, 0, &booster_) != 0 || XGBoosterLoadModel(booster_, path.c_str()) != 0) {
            if (booster_ != nullptr) XGBoosterFree(booster_);
            booster_ = nullptr;
            throw std::runtime_error("Local XGBoost artifact could not be loaded");
        }
    }
    ~NewsXGBoostModel() { if (booster_ != nullptr) XGBoosterFree(booster_); }
    NewsXGBoostModel(const NewsXGBoostModel&) = delete;
    NewsXGBoostModel& operator=(const NewsXGBoostModel&) = delete;

    [[nodiscard]] double predict(const std::vector<float>& features) const {
        const auto expected = env("ARRAKIS_XLK_NEWS_FEATURE_COUNT", std::to_string(arrakis::news::kCombinedFeatureCount));
        if (features.size() != static_cast<std::size_t>(std::stoul(expected))) throw std::runtime_error("XGBoost feature vector length does not match the active model");
        DMatrixHandle matrix = nullptr;
        if (XGDMatrixCreateFromMat(features.data(), 1, features.size(), std::numeric_limits<float>::quiet_NaN(), &matrix) != 0) {
            throw std::runtime_error("Unable to create XGBoost news feature matrix");
        }
        const char* config = R"({"type":0,"training":false,"iteration_begin":0,"iteration_end":0,"strict_shape":true})";
        const bst_ulong* shape = nullptr;
        bst_ulong dimensions = 0;
        const float* predictions = nullptr;
        const auto result = XGBoosterPredictFromDMatrix(booster_, matrix, config, &shape, &dimensions, &predictions);
        XGDMatrixFree(matrix);
        if (result != 0 || dimensions == 0 || shape == nullptr || predictions == nullptr) throw std::runtime_error("XGBoost news prediction failed");
        return std::clamp(static_cast<double>(predictions[0]), 0.0, 1.0);
    }
private:
    BoosterHandle booster_{nullptr};
};

std::string json_string(const boost::json::value& value) {
    return boost::json::serialize(value);
}

std::string iso_time(std::chrono::sys_time<std::chrono::milliseconds> value) {
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(value);
    const std::time_t raw_time = std::chrono::system_clock::to_time_t(seconds);
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

boost::json::object bar_json(const MarketBar& bar) {
    return { {"bar_id", bar.event_id}, {"symbol", bar.symbol}, {"interval", bar.interval},
        {"bar_start", iso_time(bar.bar_start)}, {"bar_end", iso_time(bar.bar_end)},
        {"open", bar.open}, {"high", bar.high}, {"low", bar.low}, {"close", bar.close},
        {"volume", bar.volume}, {"trade_count", bar.trade_count},
        {"first_trade_timestamp", iso_time(bar.first_trade_time)},
        {"last_trade_timestamp", iso_time(bar.last_trade_time)} };
}

boost::json::object error_json(std::string_view code, std::string_view message) {
    return {{"error", {{"code", code}, {"message", message}}}};
}

std::vector<std::string> split_path(std::string_view target) {
    const auto query_start = target.find('?');
    const auto path = target.substr(0, query_start);
    std::vector<std::string> output;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto part_end = end == std::string_view::npos ? path.size() : end;
        if (part_end > start) output.emplace_back(path.substr(start, part_end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return output;
}

std::string query_value(std::string_view target, std::string_view key) {
    const auto start = target.find('?');
    if (start == std::string_view::npos) return {};
    std::string_view query = target.substr(start + 1);
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto item = query.substr(0, amp);
        const auto equals = item.find('=');
        if (equals != std::string_view::npos && item.substr(0, equals) == key) return std::string(item.substr(equals + 1));
        if (amp == std::string_view::npos) break;
        query.remove_prefix(amp + 1);
    }
    return {};
}

std::optional<std::chrono::sys_time<std::chrono::milliseconds>> parse_iso(std::string value) {
    if (value.empty()) return std::nullopt;
    if (!std::regex_match(value, std::regex(R"(\d{4}-\d{2}-\d{2}(T\d{2}:\d{2}:\d{2}(\.\d{1,3})?Z)?)"))) return std::nullopt;
    if (value.size() == 10) value += "T00:00:00Z";
    std::tm tm{};
    std::istringstream input(value.substr(0, 19));
    input >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (input.fail()) return std::nullopt;
    const auto seconds = static_cast<std::int64_t>(timegm(&tm));
    return std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{seconds * 1000}};
}

std::optional<MarketBar> newest_bar(
    const arrakis::database::PostgresPool* database,
    const LiveMarketStore& market,
    std::string_view symbol,
    std::string_view interval) {
    auto newest = market.latest(symbol, interval);
    if (database != nullptr) {
        const auto persisted = database->latest_bar(symbol, interval);
        if (persisted && (!newest || persisted->bar_end > newest->bar_end)) newest = persisted;
    }
    return newest;
}

std::vector<MarketBar> merged_bars(
    const arrakis::database::PostgresPool* database,
    const LiveMarketStore& market,
    std::string_view symbol,
    std::string_view interval,
    std::optional<std::chrono::sys_time<std::chrono::milliseconds>> from,
    std::optional<std::chrono::sys_time<std::chrono::milliseconds>> to,
    std::size_t limit) {
    std::unordered_map<std::string, MarketBar> by_id;
    if (database != nullptr) {
        for (const auto& bar : database->bars(symbol, interval, from, to, limit)) {
            by_id.insert_or_assign(bar.event_id, bar);
        }
    }
    for (const auto& bar : market.bars(symbol, interval, from, to, limit)) {
        by_id.insert_or_assign(bar.event_id, bar);
    }
    std::vector<MarketBar> result;
    result.reserve(by_id.size());
    for (auto& [id, bar] : by_id) {
        static_cast<void>(id);
        result.push_back(std::move(bar));
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.bar_start < right.bar_start;
    });
    if (result.size() > limit) {
        result.erase(result.begin(), result.end() - static_cast<std::ptrdiff_t>(limit));
    }
    return result;
}

boost::json::object latest_payload(
    const arrakis::database::PostgresPool* database,
    const LiveMarketStore& market,
    std::string_view symbol) {
    const auto one = newest_bar(database, market, symbol, "1m");
    const auto five = newest_bar(database, market, symbol, "5m");
    const auto live_one = market.latest(symbol, "1m");
    const bool using_live = one && live_one && one->event_id == live_one->event_id;
    boost::json::object output{
        {"symbol", symbol},
        {"source", using_live ? "live-stream" : (database != nullptr ? "database-fallback" : "unavailable")},
    };
    if (one) output["one_minute"] = bar_json(*one); else output["one_minute"] = nullptr;
    if (five) output["five_minute"] = bar_json(*five); else output["five_minute"] = nullptr;
    return output;
}

boost::json::object live_update_json(const LiveUpdate& update) {
    return {
        {"type", "market_update"},
        {"sequence", update.sequence},
        {"trade", {
            {"event_id", update.trade.event_id},
            {"symbol", update.trade.symbol},
            {"price", update.trade.price},
            {"volume", update.trade.volume},
            {"timestamp", iso_time(std::chrono::sys_time<std::chrono::milliseconds>{
                std::chrono::milliseconds{update.trade.source_timestamp_unix_ms}})},
            {"source", update.trade.source},
        }},
        {"one_minute", bar_json(update.one_minute)},
        {"five_minute", bar_json(update.five_minute)},
    };
}

boost::json::object article_json(const arrakis::database::NewsArticle& article) {
    return {{"article_id", article.article_id}, {"url", article.canonical_url}, {"source", article.source_id}, {"headline", article.headline}, {"body", article.body}, {"published_at", iso_time(article.published_at)}, {"retrieved_at", iso_time(article.retrieved_at)}, {"novelty_score", article.novelty_score}, {"positive_probability", article.positive_probability}, {"neutral_probability", article.neutral_probability}, {"negative_probability", article.negative_probability}, {"sentiment_score", article.sentiment_score}};
}

boost::json::object news_json(const arrakis::database::NewsFeatureSnapshot& snapshot) {
    boost::json::array articles;
    for (const auto& article : snapshot.articles) articles.push_back(article_json(article));
    boost::json::value features = boost::json::object{};
    boost::system::error_code error;
    if (!snapshot.features_json.empty()) {
        features = boost::json::parse(snapshot.features_json, error);
        if (error) features = boost::json::object{};
    }
    boost::json::value warnings = boost::json::array{};
    if (!snapshot.missing_source_warnings_json.empty()) {
        warnings = boost::json::parse(snapshot.missing_source_warnings_json, error);
        if (error) warnings = boost::json::array{};
    }
    return {{"symbol", snapshot.symbol}, {"date", snapshot.trading_date}, {"publication_cutoff", snapshot.cutoff_timestamp}, {"latest_eligible_article", snapshot.latest_eligible_article_at.empty() ? boost::json::value(nullptr) : boost::json::value(snapshot.latest_eligible_article_at)}, {"coverage_status", snapshot.coverage_status.empty() ? "empty" : snapshot.coverage_status}, {"feature_schema_hash", snapshot.feature_schema_hash}, {"features", features}, {"missing_source_warnings", warnings}, {"articles", articles}, {"affected_companies", boost::json::array{}}, {"estimated_news_contribution", nullptr}, {"model_versions", {{"finbert", "finbert-v1"}, {"tokenizer", "finbert-tokenizer-v1"}, {"aggregation", "xlk-combined-features-v1"}, {"xgboost", "xlk-finbert-xgboost-v1"}}}, {"research_only_disclaimer", "Research signals only. Not investment advice. No trades are executed by this platform."}};
}

std::vector<float> news_feature_vector(const arrakis::database::NewsFeatureSnapshot& snapshot) {
    boost::system::error_code error;
    const auto parsed = boost::json::parse(snapshot.features_json, error);
    if (error || !parsed.is_object()) throw std::runtime_error("daily news features are missing or malformed");
    const auto* schema = parsed.as_object().if_contains("schema");
    if (schema == nullptr || !schema->is_string() || schema->as_string() != env("ARRAKIS_FEATURE_SCHEMA_HASH", std::string{arrakis::news::kCombinedFeatureSchemaHash})) throw std::runtime_error("daily news feature vector schema is missing or stale");
    const auto* values = parsed.as_object().if_contains("values");
    if (values == nullptr || !values->is_array() || values->as_array().size() != arrakis::news::kCombinedFeatureCount) throw std::runtime_error("daily news feature vector is missing or has the wrong length");
    std::vector<float> result;
    result.reserve(values->as_array().size());
    for (const auto& value : values->as_array()) {
        if (!value.is_double() && !value.is_int64() && !value.is_uint64()) throw std::runtime_error("daily news feature vector contains a non-numeric value");
        result.push_back(static_cast<float>(value.to_number<double>()));
    }
    return result;
}

boost::json::value route(
    const arrakis::database::PostgresPool* database,
    const LiveMarketStore& market,
    const NewsXGBoostModel* model,
    bool model_validation_required,
    const arrakis::news::FinbertSession* finbert,
    const RuntimeState& runtime,
    std::string_view target,
    unsigned& status) {
    const auto path = split_path(target);
    const bool database_healthy = database != nullptr && database->healthy();
    const auto* fallback_database = database_healthy ? database : nullptr;
    if (target == "/health") return {{"status", "ok"}};
    if (target == "/ready") {
        const bool live_data_available = market.sequence() != 0;
        const bool ready = live_data_available || database_healthy;
        if (!ready) status = 503;
        return {
            {"status", ready ? "ready" : "not_ready"},
            {"market_stream", runtime.kafka_consumer_running.load() ? (live_data_available ? "live" : "listening") : "unavailable"},
            {"database_fallback", database_healthy ? "available" : "unavailable"},
            {"market_data_available", ready},
        };
    }
    if (target == "/api/v1/system/status") {
        std::optional<MarketBar> latest;
        bool live_data = false;
        const auto etfs = market.etfs();
        for (const auto& etf : etfs) {
            const auto live = market.latest(etf.symbol, "1m");
            live_data = live_data || live.has_value();
            const auto bar = newest_bar(fallback_database, market, etf.symbol, "1m");
            if (bar && (!latest || bar->bar_end > latest->bar_end)) latest = bar;
        }
        const auto now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto age = latest ? std::chrono::duration_cast<std::chrono::seconds>(now - std::chrono::time_point_cast<std::chrono::seconds>(latest->bar_end)).count() : -1;
        return {{"database", database_healthy ? "ml-ready" : "unavailable"}, {"market_data_source", live_data ? "finnhub-websocket-via-kafka+database" : (database_healthy ? "database-fallback" : "finnhub-websocket-via-kafka")}, {"market_data_status", latest ? (age <= 120 ? "fresh" : "stale") : "waiting_for_stream"}, {"latest_bar_end", latest ? boost::json::value(iso_time(latest->bar_end)) : boost::json::value(nullptr)}, {"latest_bar_age_seconds", latest ? boost::json::value(age) : boost::json::value(nullptr)}, {"active_etfs", etfs.size()}, {"ml_available", model != nullptr}};
    }
    if (path.size() >= 5 && path[0] == "api" && path[1] == "v1" && path[2] == "etfs" && path[3] == "XLK" && (path[4] == "news" || path[4] == "nlp-features" || path[4] == "insights" || path[4] == "prediction")) {
        const auto date = query_value(target, "date");
        if (!std::regex_match(date, std::regex(R"(\d{4}-\d{2}-\d{2})"))) { status = 400; return error_json("INVALID_DATE", "date must be YYYY-MM-DD."); }
        if (!database_healthy) {
            status = 503;
            return error_json("ML_DATABASE_UNAVAILABLE", "The ML feature database is unavailable; live market data is unaffected.");
        }
        const auto snapshot = database->news_snapshot("XLK", date);
        if (!snapshot.feature_schema_hash.empty() && snapshot.feature_schema_hash != env("ARRAKIS_FEATURE_SCHEMA_HASH", std::string{arrakis::news::kCombinedFeatureSchemaHash})) { status = 409; return error_json("FEATURE_SCHEMA_MISMATCH", "Stored news features do not match the active model schema."); }
        if (path[4] == "news" || path[4] == "nlp-features") return news_json(snapshot);
        if (model == nullptr || finbert == nullptr || !finbert->ready()) {
            status = 503;
            return error_json(
                model_validation_required ? "NO_VALIDATED_MODEL" : "MODEL_UNAVAILABLE",
                model_validation_required
                    ? "No validated XLK model is enabled; no fallback prediction is available."
                    : "The versioned FinBERT ONNX and XGBoost XLK artifacts are not available."
            );
        }
        std::vector<float> features;
        try {
            features = news_feature_vector(snapshot);
        } catch (const std::exception& error) {
            status = 409;
            return error_json("FEATURE_SCHEMA_MISMATCH", error.what());
        }
        const auto probability = model->predict(features);
        const auto signal = probability >= 0.5 ? "Bullish" : "Bearish";
        status = 200;
        auto insight = news_json(snapshot);
        insight["prediction"] = {{"direction", signal}, {"probability_positive_return", probability}, {"threshold", 0.5}, {"model_id", "xlk-finbert-xgboost-v1"}};
        insight["dominant_themes"] = boost::json::array{"technology", "semiconductors", "software"};
        insight["why_model_moved"] = "Feature attribution is limited to persisted article and aggregate features; no unsupported explanation is generated.";
        return insight;
    }
    if (target == "/api/v1/recommendation" || target.starts_with("/api/v1/recommendation?")) { status = 410; return error_json("ENDPOINT_RETIRED", "Use the date-scoped XLK prediction endpoint."); }
    if (target == "/api/v1/etfs") {
        boost::json::array data;
        for (const auto& etf : market.etfs()) data.push_back({{"symbol", etf.symbol}, {"name", etf.name}, {"category", etf.category}, {"active", etf.active}});
        return {{"data", data}};
    }
    if (target == "/api/v1/market/snapshot") {
        boost::json::array data;
        bool used_live = false;
        bool used_database = false;
        for (const auto& etf : market.etfs()) {
            const auto live = market.latest(etf.symbol, "1m");
            const auto persisted = fallback_database != nullptr
                ? fallback_database->latest_bar(etf.symbol, "1m") : std::nullopt;
            const auto latest = persisted && (!live || persisted->bar_end > live->bar_end)
                ? persisted : live;
            const bool from_database = latest && persisted && latest->event_id == persisted->event_id;
            used_database = used_database || from_database;
            used_live = used_live || (latest && !from_database);
            data.push_back({{"symbol", etf.symbol}, {"name", etf.name}, {"category", etf.category}, {"source", from_database ? "database-fallback" : "live-stream"}, {"bar", latest ? boost::json::value(bar_json(*latest)) : boost::json::value(nullptr)}});
        }
        const auto source = used_live && used_database ? "live+database-fallback"
            : (used_database ? "database-fallback" : "finnhub-websocket-via-kafka");
        return {{"source", source}, {"data", data}};
    }
    if (path.size() >= 4 && path[0] == "api" && path[1] == "v1" && path[2] == "etfs") {
        const std::string symbol = path[3];
        if (!market.supports(symbol)) { status = 404; return error_json("ETF_NOT_FOUND", "ETF symbol is not supported."); }
        if (path.size() == 5 && path[4] == "latest") return latest_payload(fallback_database, market, symbol);
        if (path.size() == 5 && (path[4] == "bars" || path[4] == "intraday")) {
            const std::string interval = query_value(target, "interval").empty() ? "1m" : query_value(target, "interval");
            if (interval != "1m" && interval != "5m") { status = 400; return error_json("INVALID_INTERVAL", "interval must be 1m or 5m."); }
            const auto from = parse_iso(query_value(target, "from"));
            const auto to = parse_iso(query_value(target, "to"));
            const auto date = parse_iso(query_value(target, "date"));
            if ((!query_value(target, "from").empty() && !from) ||
                (!query_value(target, "to").empty() && !to) ||
                (!query_value(target, "date").empty() && !date)) {
                status = 400;
                return error_json("INVALID_TIMESTAMP", "from, to, and date must be ISO UTC timestamps.");
            }
            const auto start = date ? date : from;
            const auto end = date ? std::optional<std::chrono::sys_time<std::chrono::milliseconds>>(*date + std::chrono::hours(24)) : to;
            const auto limit_text = query_value(target, "limit");
            const auto limit = limit_text.empty() ? std::size_t{120} : static_cast<std::size_t>(std::stoul(limit_text));
            if (limit == 0 || limit > 2000) { status = 400; return error_json("INVALID_LIMIT", "limit must be between 1 and 2000."); }
            boost::json::array data;
            const auto live = market.bars(symbol, interval, start, end, limit);
            for (const auto& bar : merged_bars(fallback_database, market, symbol, interval, start, end, limit)) data.push_back(bar_json(bar));
            const auto source = fallback_database != nullptr
                ? (live.empty() ? "database-fallback" : "live+database-fallback")
                : "finnhub-websocket-via-kafka";
            return {{"symbol", symbol}, {"interval", interval}, {"source", source}, {"data", data}};
        }
    }
    status = 404;
    return error_json("NOT_FOUND", "Route not found.");
}

http::response<http::string_body> handle(
    const arrakis::database::PostgresPool* database,
    const LiveMarketStore& market,
    const NewsXGBoostModel* model,
    bool model_validation_required,
    const arrakis::news::FinbertSession* finbert,
    RuntimeState& runtime,
    const http::request<http::string_body>& request) {
    runtime.api_requests.fetch_add(1);
    if (request.method() != http::verb::get) {
        http::response<http::string_body> response{http::status::method_not_allowed, request.version()};
        response.set(http::field::content_type, "application/json");
        response.set(http::field::allow, "GET, OPTIONS");
        response.body() = json_string(error_json("METHOD_NOT_ALLOWED", "Only GET requests are supported."));
        response.prepare_payload();
        return response;
    }
    if (request.target() == "/metrics") {
        http::response<http::string_body> response{http::status::ok, request.version()};
        response.set(http::field::content_type, "text/plain; version=0.0.4");
        response.set(http::field::access_control_allow_origin, env("CORS_ALLOWED_ORIGINS", "http://localhost:3000"));
        std::int64_t latest_age_seconds = -1;
        for (const auto& etf : market.etfs()) {
            const auto bar = market.latest(etf.symbol, "1m");
            if (!bar) continue;
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now() - bar->bar_end).count();
            if (latest_age_seconds < 0 || age < latest_age_seconds) latest_age_seconds = age;
        }
        response.body() =
            "# TYPE api_requests_total counter\napi_requests_total " +
            std::to_string(runtime.api_requests.load()) +
            "\n# TYPE market_api_connection_errors_total counter\nmarket_api_connection_errors_total " +
            std::to_string(runtime.connection_errors.load()) +
            "\n# TYPE market_api_kafka_records_total counter\nmarket_api_kafka_records_total " +
            std::to_string(runtime.kafka_records.load()) +
            "\n# TYPE market_api_kafka_rejected_records_total counter\nmarket_api_kafka_rejected_records_total " +
            std::to_string(runtime.kafka_rejected_records.load()) +
            "\n# TYPE market_api_kafka_consumer_running gauge\nmarket_api_kafka_consumer_running " +
            std::to_string(runtime.kafka_consumer_running.load() ? 1 : 0) +
            "\n# TYPE latest_live_market_bar_age_seconds gauge\nlatest_live_market_bar_age_seconds " +
            std::to_string(latest_age_seconds) + "\n";
        response.prepare_payload();
        return response;
    }
    unsigned status = 200;
    boost::json::value body;
    try { body = route(database, market, model, model_validation_required, finbert, runtime, std::string_view(request.target().data(), request.target().size()), status); }
    catch (const std::invalid_argument& error) { status = 400; body = error_json("INVALID_REQUEST", error.what()); }
    catch (const std::exception& error) { status = 500; body = error_json("INTERNAL_ERROR", error.what()); }
    http::response<http::string_body> response{static_cast<http::status>(status), request.version()};
    response.set(http::field::content_type, "application/json");
    response.set(http::field::access_control_allow_origin, env("CORS_ALLOWED_ORIGINS", "http://localhost:3000"));
    response.set(http::field::access_control_allow_methods, "GET,OPTIONS");
    response.set(http::field::access_control_allow_headers, "Content-Type,Authorization,X-Request-Id");
    response.set("X-Request-Id", "market-api-" + std::to_string(request_sequence.fetch_add(1) + 1));
    response.body() = json_string(body);
    response.prepare_payload();
    return response;
}

void serve_live_websocket(
    tcp::socket socket,
    http::request<http::string_body> request,
    const LiveMarketStore& market,
    RuntimeState& runtime) {
    try {
        websocket::stream<tcp::socket> stream(std::move(socket));
        stream.set_option(websocket::stream_base::timeout::suggested(boost::beast::role_type::server));
        stream.set_option(websocket::stream_base::decorator([](websocket::response_type& response) {
            response.set(http::field::server, "arrakis-market-api");
        }));
        stream.accept(request);
        stream.text(true);

        std::uint64_t sequence = market.sequence();
        if (const auto initial = market.last_update()) {
            sequence = initial->sequence;
            const auto payload = json_string(live_update_json(*initial));
            stream.write(net::buffer(payload));
        } else {
            const auto payload = json_string(boost::json::object{
                {"type", "stream_status"},
                {"status", "waiting_for_market_data"},
                {"source", "finnhub-websocket-via-kafka"},
            });
            stream.write(net::buffer(payload));
        }

        for (;;) {
            LiveUpdate update;
            if (market.wait_for_update(sequence, std::chrono::seconds(15), update)) {
                sequence = update.sequence;
                const auto payload = json_string(live_update_json(update));
                stream.write(net::buffer(payload));
            } else {
                const auto heartbeat = json_string(boost::json::object{
                    {"type", "heartbeat"},
                    {"sequence", sequence},
                    {"source", "finnhub-websocket-via-kafka"},
                });
                stream.write(net::buffer(heartbeat));
            }
        }
    } catch (const boost::system::system_error& error) {
        if (error.code() != websocket::error::closed &&
            error.code() != boost::asio::error::operation_aborted &&
            error.code() != boost::asio::error::connection_reset &&
            error.code() != boost::asio::error::broken_pipe) {
            runtime.connection_errors.fetch_add(1);
            std::cerr << "{\"service\":\"market-api\",\"event\":\"websocket_closed\",\"error\":\""
                      << error.what() << "\"}\n";
        }
    } catch (const std::exception& error) {
        runtime.connection_errors.fetch_add(1);
        std::cerr << "{\"service\":\"market-api\",\"event\":\"websocket_connection_error\",\"error\":\""
                  << error.what() << "\"}\n";
    }
}

std::unique_ptr<arrakis::database::PostgresPool> connect_database() {
    try {
        return std::make_unique<arrakis::database::PostgresPool>(
            arrakis::database::database_config_from_environment());
    } catch (const std::exception& error) {
        std::cerr << "{\"service\":\"market-api\",\"event\":\"ml_database_unavailable\",\"error\":\""
                  << error.what() << "\"}\n";
        return nullptr;
    }
}

}  // namespace

int main() {
    try {
        RuntimeState runtime;
        auto database = connect_database();
        auto next_database_retry = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        LiveMarketStore market(env("ARRAKIS_ETF_UNIVERSE", "config/etf_universe.json"));
        std::jthread market_consumer([&market, &runtime](std::stop_token stop_token) {
            try {
                arrakis::streaming::KafkaConsumer consumer(
                    env("KAFKA_BOOTSTRAP_SERVERS", "localhost:9092"),
                    env("MARKET_API_CONSUMER_GROUP", "market-api-live-v1"),
                    env("MARKET_RAW_TOPIC", "market.raw.trades"));
                runtime.kafka_consumer_running.store(true);
                while (!stop_token.stop_requested()) {
                    const auto record = consumer.poll(std::chrono::milliseconds(250));
                    if (!record) continue;
                    try {
                        const auto trade = arrakis::streaming::deserialize_trade(record->payload);
                        static_cast<void>(market.apply(trade));
                        runtime.kafka_records.fetch_add(1);
                    } catch (const std::exception& error) {
                        runtime.kafka_rejected_records.fetch_add(1);
                        std::cerr << "{\"service\":\"market-api\",\"event\":\"live_trade_rejected\",\"error\":\""
                                  << error.what() << "\"}\n";
                    }
                    consumer.commit(*record);
                }
            } catch (const std::exception& error) {
                runtime.kafka_consumer_running.store(false);
                std::cerr << "{\"service\":\"market-api\",\"event\":\"live_stream_unavailable\",\"error\":\""
                          << error.what() << "\"}\n";
            }
        });
        std::unique_ptr<NewsXGBoostModel> model;
        const bool model_validation_required = !env_true("ARRAKIS_XLK_NEWS_MODEL_VALIDATED");
        try { model = std::make_unique<NewsXGBoostModel>(); }
        catch (const std::exception& error) { std::cerr << "{\"service\":\"market-api\",\"event\":\"model_unavailable\",\"error\":\"" << error.what() << "\"}\n"; }
        std::unique_ptr<arrakis::news::FinbertSession> finbert;
        try { finbert = std::make_unique<arrakis::news::FinbertSession>(env("ARRAKIS_FINBERT_ONNX_PATH"), env("ARRAKIS_FINBERT_VOCAB_PATH"), env("ARRAKIS_FINBERT_VERSION", "finbert-v1"), env("ARRAKIS_FINBERT_TOKENIZER_VERSION", "finbert-tokenizer-v1"), static_cast<std::size_t>(std::stoul(env("ARRAKIS_FINBERT_MAX_TOKENS", "128")))); }
        catch (const std::exception& error) { std::cerr << "{\"service\":\"market-api\",\"event\":\"finbert_unavailable\",\"error\":\"" << error.what() << "\"}\n"; }
        net::io_context io;
        tcp::acceptor acceptor(io, {tcp::v4(), static_cast<unsigned short>(std::stoul(env("MARKET_API_PORT", "8080")))});
        std::cerr << "{\"service\":\"market-api\",\"event\":\"started\"}\n";
        for (;;) {
            tcp::socket socket(io);
            boost::system::error_code error;
            acceptor.accept(socket, error);
            if (error) {
                runtime.connection_errors.fetch_add(1);
                std::cerr << "{\"service\":\"market-api\",\"event\":\"accept_error\",\"error\":\""
                          << error.message() << "\"}\n";
                continue;
            }
            if (!database && std::chrono::steady_clock::now() >= next_database_retry) {
                database = connect_database();
                next_database_retry = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            }
            boost::beast::tcp_stream transport(std::move(socket));
            transport.expires_after(std::chrono::seconds(10));
            boost::beast::flat_buffer buffer;
            http::request<http::string_body> request;
            http::read(transport, buffer, request, error);
            if (error) {
                if (error != http::error::end_of_stream &&
                    error != boost::asio::error::operation_aborted &&
                    error != boost::asio::error::connection_reset) {
                    runtime.connection_errors.fetch_add(1);
                    std::cerr << "{\"service\":\"market-api\",\"event\":\"http_read_error\",\"error\":\""
                              << error.message() << "\"}\n";
                }
                continue;
            }
            if (websocket::is_upgrade(request) && request.target() == "/ws/v1/market") {
                std::thread(
                    [stream_socket = transport.release_socket(),
                     stream_request = std::move(request),
                     &market,
                     &runtime]() mutable {
                        serve_live_websocket(
                            std::move(stream_socket), std::move(stream_request), market, runtime);
                    })
                    .detach();
                continue;
            }
            auto response = request.method() == http::verb::options
                ? http::response<http::string_body>{http::status::no_content, request.version()}
                : handle(database.get(), market, model.get(), model_validation_required, finbert.get(), runtime, request);
            if (request.method() == http::verb::options) runtime.api_requests.fetch_add(1);
            response.set(http::field::access_control_allow_origin, env("CORS_ALLOWED_ORIGINS", "http://localhost:3000"));
            response.set(http::field::access_control_allow_methods, "GET,OPTIONS");
            response.set(http::field::access_control_allow_headers, "Content-Type,Authorization,X-Request-Id");
            response.keep_alive(false);
            transport.expires_after(std::chrono::seconds(10));
            http::write(transport, response, error);
            if (error && error != boost::asio::error::connection_reset &&
                error != boost::asio::error::broken_pipe) {
                runtime.connection_errors.fetch_add(1);
                std::cerr << "{\"service\":\"market-api\",\"event\":\"http_write_error\",\"error\":\""
                          << error.message() << "\"}\n";
            }
            transport.socket().shutdown(tcp::socket::shutdown_send, error);
        }
    } catch (const std::exception& error) {
        std::cerr << "{\"service\":\"market-api\",\"event\":\"fatal\",\"error\":\"" << error.what() << "\"}\n";
        return EXIT_FAILURE;
    }
}
