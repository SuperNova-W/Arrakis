#include "arrakis/serialization/news_serialization.hpp"
#include "arrakis/historical_data/historical_data.hpp"
#include "arrakis/streaming/kafka.hpp"

#include <boost/json.hpp>
#include <openssl/sha.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>

namespace {
std::string env(const char* name, std::string fallback = {}) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::move(fallback) : std::string(value);
}

std::int64_t env_int(const char* name, std::int64_t fallback) {
    const auto value = env(name);
    return value.empty() ? fallback : std::stoll(value);
}

std::string utc_date(std::chrono::system_clock::time_point value) {
    const auto time = std::chrono::system_clock::to_time_t(value);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%d");
    return output.str();
}

std::string sha256(std::string_view value) {
    unsigned char digest[SHA256_DIGEST_LENGTH]{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
}

std::string required_string(const boost::json::object& object, std::string_view key) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || !value->is_string() || value->as_string().empty()) throw std::invalid_argument("news article missing " + std::string(key));
    return std::string(value->as_string());
}

std::int64_t required_int(const boost::json::object& object, std::string_view key) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || !value->is_int64() || value->as_int64() <= 0) throw std::invalid_argument("news article has invalid " + std::string(key));
    return value->as_int64();
}

arrakis::news::Article parse(std::string_view line) {
    boost::system::error_code error;
    const auto value = boost::json::parse(line, error);
    if (error || !value.is_object()) throw std::invalid_argument("news fixture line is not a JSON object");
    const auto& object = value.as_object();
    arrakis::news::Article article;
    article.canonical_url = required_string(object, "canonical_url");
    article.source_id = required_string(object, "source_id");
    article.headline = required_string(object, "headline");
    article.body = object.if_contains("body") && object.at("body").is_string() ? std::string(object.at("body").as_string()) : article.headline;
    article.published_at_unix_ms = required_int(object, "published_at_unix_ms");
    article.retrieved_at_unix_ms = required_int(object, "retrieved_at_unix_ms");
    article.language = object.if_contains("language") && object.at("language").is_string() ? std::string(object.at("language").as_string()) : "en";
    article.normalized_content_hash = sha256(article.headline + "\n" + article.body);
    article.article_id = "news:sha256:" + sha256(article.canonical_url + "\n" + article.normalized_content_hash);
    if (const auto* entities = object.if_contains("entities"); entities != nullptr && entities->is_array()) {
        for (const auto& entity : entities->as_array()) if (entity.is_string()) article.entity_ids.emplace_back(entity.as_string());
    }
    if (article.entity_ids.empty()) article.entity_ids.emplace_back("XLK");
    return article;
}

arrakis::news::Article from_finnhub(const arrakis::historical_data::NewsStory& story, std::string_view symbol) {
    arrakis::news::Article article;
    article.canonical_url = story.url; article.source_id = story.source.empty() ? "finnhub-company-news" : story.source; article.headline = story.headline; article.body = story.summary; article.published_at_unix_ms = story.published_at_unix_seconds * 1000; article.retrieved_at_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); article.normalized_content_hash = sha256(article.headline + "\n" + article.body); article.article_id = "news:sha256:" + sha256(article.canonical_url + "\n" + article.normalized_content_hash); article.entity_ids = {std::string(symbol), "sector:technology"}; return article;
}
}

int main(int argc, char** argv) {
    try {
        arrakis::streaming::KafkaProducer producer(env("KAFKA_BOOTSTRAP_SERVERS", "localhost:9092"), "news-ingestion-v1");
        std::uint64_t published = 0;
        std::unordered_set<std::string> published_ids;
        const auto publish = [&](const arrakis::news::Article& article) {
            if (published_ids.size() >= 10000) published_ids.clear();
            if (!published_ids.insert(article.article_id).second) return;
            const auto payload = arrakis::news::serialize_article(article);
            producer.publish(env("NEWS_RAW_TOPIC", "news.raw.articles"), "XLK", payload);
            producer.poll_events(std::chrono::milliseconds{0});
            ++published;
        };
        if (argc == 3 && std::string_view(argv[1]) == "--fixture") {
            std::ifstream input(argv[2]);
            if (!input) throw std::runtime_error("cannot open news fixture");
            std::string line;
            while (std::getline(input, line)) if (!line.empty()) publish(parse(line));
        } else if (argc == 5 && std::string_view(argv[1]) == "--finnhub") {
            arrakis::historical_data::FinnhubClient client({.api_key = env("FINNHUB_API_KEY")});
            for (const auto& story : client.get_company_news(argv[2], argv[3], argv[4])) publish(from_finnhub(story, argv[2]));
        } else if (argc == 3 && std::string_view(argv[1]) == "--finnhub-poll") {
            const std::string symbol = argv[2];
            const auto interval = std::chrono::seconds{env_int("NEWS_POLL_INTERVAL_SECONDS", 900)};
            const auto lookback = std::chrono::hours{24 * env_int("NEWS_POLL_LOOKBACK_DAYS", 3)};
            if (interval.count() <= 0 || lookback.count() <= 0) throw std::invalid_argument("NEWS_POLL_INTERVAL_SECONDS and NEWS_POLL_LOOKBACK_DAYS must be positive");
            for (;;) {
                const auto now = std::chrono::system_clock::now();
                const auto from = utc_date(now - lookback);
                const auto to = utc_date(now);
                arrakis::historical_data::FinnhubClient client({.api_key = env("FINNHUB_API_KEY")});
                for (const auto& story : client.get_company_news(symbol, from, to)) publish(from_finnhub(story, symbol));
                producer.flush(std::chrono::seconds{10});
                std::cerr << "{\"service\":\"news-ingestion\",\"event\":\"poll_complete\",\"symbol\":\""
                          << symbol << "\",\"published_total\":" << published << "}\n";
                std::this_thread::sleep_for(interval);
            }
        } else {
            throw std::invalid_argument("Usage: news-ingestion --fixture <jsonl> | --finnhub <symbol> <from YYYY-MM-DD> <to YYYY-MM-DD> | --finnhub-poll <symbol>");
        }
        producer.flush(std::chrono::seconds{10});
        std::cout << "published_news_articles=" << published << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "{\"service\":\"news-ingestion\",\"error\":\"" << error.what() << "\"}\n";
        return EXIT_FAILURE;
    }
}
