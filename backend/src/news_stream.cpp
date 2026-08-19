#include "arrakis/serialization/news_serialization.hpp"
#include "arrakis/historical_data/historical_data.hpp"
#include "arrakis/news/xlk_membership.hpp"
#include "arrakis/streaming/kafka.hpp"

#include <boost/json.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <map>
#include <thread>
#include <unordered_set>
#include <vector>

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

// Article carries the ETF, the constituent that surfaced it, and the sector, so
// the enricher's entity-weighted features have something to key on. Training
// data is built from constituent-company news, so the live tags must name the
// constituent too, not just the ETF.
arrakis::news::Article from_constituent_news(
    const arrakis::historical_data::NewsStory& story,
    std::string_view etf,
    std::string_view constituent
) {
    auto article = from_finnhub(story, etf);
    article.entity_ids = {std::string(etf), "company:" + std::string(constituent), "sector:technology"};
    return article;
}

std::string json_escape(std::string_view value) {
    std::string output;
    for (const auto character : value) {
        if (character == '"' || character == '\\') output.push_back('\\');
        if (static_cast<unsigned char>(character) < 0x20) { output.push_back(' '); continue; }
        output.push_back(character);
    }
    return output;
}

bool is_rate_limited(const std::exception& error) {
    return std::string_view{error.what()}.find("429") != std::string_view::npos;
}

// Finnhub's free tier allows roughly 60 calls/minute. With ~57 pollable
// constituents plus the ETF we need one full pass per poll interval (900s by
// default), so a fixed inter-request delay of ~1.5s finishes a pass in about 90
// seconds while staying at ~40 calls/minute. Slow and correct beats fast here.
std::vector<arrakis::historical_data::NewsStory> fetch_with_backoff(
    arrakis::historical_data::FinnhubClient& client,
    const std::string& symbol,
    const std::string& from,
    const std::string& to,
    const int max_attempts,
    std::chrono::milliseconds backoff,
    const std::chrono::milliseconds max_backoff
) {
    for (int attempt = 1;; ++attempt) {
        try {
            return client.get_company_news(symbol, from, to);
        } catch (const std::exception& error) {
            const bool throttled = is_rate_limited(error);
            if (!throttled || attempt >= max_attempts) throw;
            std::cerr << "{\"service\":\"news-ingestion\",\"event\":\"rate_limited\",\"symbol\":\""
                      << json_escape(symbol) << "\",\"attempt\":" << attempt
                      << ",\"backoff_ms\":" << backoff.count() << "}\n";
            std::this_thread::sleep_for(backoff);
            backoff = std::min(backoff * 2, max_backoff);
        }
    }
}

// Today's constituents, plus the ETF itself. Filing artifacts (rights,
// when-issued and preferred lines such as HPE-PC or ORCL-PD) have no company
// news feed and are skipped rather than burning a request every cycle.
std::vector<std::string> poll_universe(
    const arrakis::news::XlkMembershipResolver& membership,
    const std::string& etf,
    const std::string& date,
    std::size_t& skipped
) {
    std::vector<std::string> universe{etf};
    skipped = 0;
    for (const auto& member : membership.constituents_on(date)) {
        if (member.symbol == etf) continue;
        if (!arrakis::news::is_pollable_ticker(member.symbol)) { ++skipped; continue; }
        universe.push_back(member.symbol);
    }
    return universe;
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
            const auto request_spacing = std::chrono::milliseconds{env_int("NEWS_POLL_REQUEST_SPACING_MS", 1500)};
            const auto max_attempts = static_cast<int>(env_int("NEWS_POLL_MAX_ATTEMPTS", 5));
            const auto initial_backoff = std::chrono::milliseconds{env_int("NEWS_POLL_BACKOFF_MS", 5000)};
            const auto max_backoff = std::chrono::milliseconds{env_int("NEWS_POLL_MAX_BACKOFF_MS", 120000)};
            if (interval.count() <= 0 || lookback.count() <= 0) throw std::invalid_argument("NEWS_POLL_INTERVAL_SECONDS and NEWS_POLL_LOOKBACK_DAYS must be positive");
            if (request_spacing.count() < 0 || max_attempts < 1 || initial_backoff.count() <= 0) throw std::invalid_argument("NEWS_POLL_REQUEST_SPACING_MS must be non-negative, NEWS_POLL_MAX_ATTEMPTS >= 1, NEWS_POLL_BACKOFF_MS positive");

            const auto history_path = arrakis::news::XlkMembershipResolver::default_history_path();
            const auto membership = arrakis::news::XlkMembershipResolver::from_csv(history_path);
            std::cerr << "{\"service\":\"news-ingestion\",\"event\":\"membership_loaded\",\"path\":\""
                      << json_escape(history_path.string()) << "\",\"first_snapshot\":\""
                      << membership.first_snapshot_date() << "\",\"last_snapshot\":\""
                      << membership.last_snapshot_date() << "\"}\n";

            for (;;) {
                const auto now = std::chrono::system_clock::now();
                const auto from = utc_date(now - lookback);
                const auto to = utc_date(now);
                std::size_t skipped = 0;
                const auto universe = poll_universe(membership, symbol, to, skipped);
                if (universe.size() == 1) {
                    // Only the ETF itself resolved: the holdings history does not
                    // cover today. Say so instead of quietly degrading to the old
                    // single-ticker behaviour.
                    std::cerr << "{\"service\":\"news-ingestion\",\"event\":\"no_constituents_resolved\""
                              << ",\"date\":\"" << to << "\",\"first_snapshot\":\""
                              << membership.first_snapshot_date() << "\"}\n";
                }
                if (membership.is_extrapolated_forward(to)) {
                    std::cerr << "{\"service\":\"news-ingestion\",\"event\":\"membership_extrapolated\""
                              << ",\"date\":\"" << to << "\",\"last_snapshot\":\""
                              << membership.last_snapshot_date()
                              << "\",\"note\":\"newest N-PORT filing carried forward; membership may be stale\"}\n";
                }

                arrakis::historical_data::FinnhubClient client({.api_key = env("FINNHUB_API_KEY")});
                std::size_t requested = 0;
                std::size_t failed = 0;
                // The same article is often returned for several constituents.
                // Merge their entity tags before publishing so a story about
                // both MSFT and NVDA is tagged with both.
                std::map<std::string, arrakis::news::Article> batch;
                for (const auto& ticker : universe) {
                    if (requested > 0 && request_spacing.count() > 0) std::this_thread::sleep_for(request_spacing);
                    ++requested;
                    try {
                        const auto stories = fetch_with_backoff(client, ticker, from, to, max_attempts, initial_backoff, max_backoff);
                        for (const auto& story : stories) {
                            auto article = ticker == symbol ? from_finnhub(story, symbol)
                                                            : from_constituent_news(story, symbol, ticker);
                            const auto [entry, inserted] = batch.try_emplace(article.article_id, std::move(article));
                            if (inserted) continue;
                            const auto tag = "company:" + ticker;
                            if (ticker != symbol && std::ranges::find(entry->second.entity_ids, tag) == entry->second.entity_ids.end()) {
                                entry->second.entity_ids.push_back(tag);
                            }
                        }
                    } catch (const std::exception& error) {
                        // One bad ticker must not kill a long-running poller.
                        ++failed;
                        std::cerr << "{\"service\":\"news-ingestion\",\"event\":\"fetch_failed\",\"symbol\":\""
                                  << json_escape(ticker) << "\",\"error\":\"" << json_escape(error.what()) << "\"}\n";
                    }
                }
                for (const auto& [article_id, article] : batch) {
                    static_cast<void>(article_id);
                    publish(article);
                }
                producer.flush(std::chrono::seconds{10});
                std::cerr << "{\"service\":\"news-ingestion\",\"event\":\"poll_complete\",\"symbol\":\""
                          << symbol << "\",\"constituents\":" << universe.size() - 1
                          << ",\"skipped_non_pollable\":" << skipped
                          << ",\"requests\":" << requested << ",\"failed_requests\":" << failed
                          << ",\"unique_articles\":" << batch.size()
                          << ",\"published_total\":" << published << "}\n";
                std::this_thread::sleep_for(interval);
            }
        } else {
            throw std::invalid_argument("Usage: news-ingestion --fixture <jsonl> | --finnhub <symbol> <from YYYY-MM-DD> <to YYYY-MM-DD> | --finnhub-poll <etf-symbol> (polls the ETF plus its point-in-time constituents)");
        }
        producer.flush(std::chrono::seconds{10});
        std::cout << "published_news_articles=" << published << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "{\"service\":\"news-ingestion\",\"error\":\"" << error.what() << "\"}\n";
        return EXIT_FAILURE;
    }
}
