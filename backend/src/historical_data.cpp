#include "arrakis/historical_data/historical_data.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arrakis::historical_data {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

[[nodiscard]] std::string read_environment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || std::string_view{value}.empty()) {
        throw std::runtime_error{"Missing environment variable: " + std::string{name}};
    }
    return value;
}

[[nodiscard]] std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

[[nodiscard]] std::chrono::system_clock::time_point to_time_point(const std::chrono::seconds seconds) {
    return std::chrono::system_clock::time_point{seconds};
}

[[nodiscard]] std::string utc_now_iso() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);
    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

[[nodiscard]] std::string to_iso(const std::chrono::system_clock::time_point& point) {
    const auto time = std::chrono::system_clock::to_time_t(point);
    std::tm tm{};
    gmtime_r(&time, &tm);
    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

[[nodiscard]] std::chrono::system_clock::time_point from_iso(const std::string& value) {
    std::tm tm{};
    std::istringstream stream{value};
    stream >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (!stream) {
        throw std::runtime_error{"Invalid timestamp: " + value};
    }
    const auto time = timegm(&tm);
    return to_time_point(std::chrono::seconds{time});
}

[[nodiscard]] std::string safe_string(const boost::json::value& value, const char* key) {
    const auto* object = value.if_object();
    if (object == nullptr) {
        return {};
    }
    if (const auto it = object->find(key); it != object->end()) {
        if (const auto* str = it->value().if_string()) {
            return std::string{*str};
        }
    }
    return {};
}

[[nodiscard]] std::string fetch_http_body(
    std::string_view host,
    std::string_view target,
    std::chrono::seconds /*timeout*/
) {
    asio::io_context io_context;
    ssl::context tls_context(ssl::context::tls_client);
    tls_context.set_default_verify_paths();
    tls_context.set_verify_mode(ssl::verify_peer);

    tcp::resolver resolver(io_context);
    ssl::stream<tcp::socket> stream(io_context, tls_context);

    const auto endpoints = resolver.resolve(std::string(host), "443");
    asio::connect(stream.next_layer(), endpoints.begin(), endpoints.end());

    if (!SSL_set_tlsext_host_name(stream.native_handle(), std::string(host).c_str())) {
        throw std::runtime_error{"Unable to configure TLS server name"};
    }

    stream.handshake(ssl::stream_base::client);
    http::request<http::string_body> request{http::verb::get, std::string(target), 11};
    request.set(http::field::host, std::string(host));
    request.set(http::field::user_agent, "Arrakis/0.1");
    request.set(http::field::accept, "application/json");
    request.set(http::field::connection, "close");

    http::write(stream, request);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    stream.shutdown();

    if (response.result() == http::status::moved_permanently || response.result() == http::status::found ||
        response.result() == http::status::temporary_redirect || response.result() == http::status::permanent_redirect) {
        if (const auto location = response.find(http::field::location); location != response.end()) {
            const auto redirected_target = std::string{location->value()};
            return fetch_http_body(host, redirected_target, std::chrono::seconds{30});
        }
    }

    if (response.result() != http::status::ok) {
        throw std::runtime_error{"Finnhub request failed with HTTP status " + std::to_string(static_cast<int>(response.result()))};
    }

    return response.body();
}

[[nodiscard]] std::int64_t json_integer(const boost::json::value& value) {
    if (value.is_int64()) return value.as_int64();
    if (value.is_uint64()) return static_cast<std::int64_t>(value.as_uint64());
    throw std::runtime_error{"Expected an integer in market-data response"};
}

[[nodiscard]] double json_number_or_nan(const boost::json::value& value) {
    if (value.is_double()) return value.as_double();
    if (value.is_int64()) return static_cast<double>(value.as_int64());
    if (value.is_uint64()) return static_cast<double>(value.as_uint64());
    if (value.is_null()) return std::numeric_limits<double>::quiet_NaN();
    throw std::runtime_error{"Expected a number in market-data response"};
}

[[nodiscard]] std::vector<std::int64_t> parse_integer_array(const boost::json::value& source) {
    std::vector<std::int64_t> output;
    if (!source.is_array()) return output;
    output.reserve(source.as_array().size());
    for (const auto& item : source.as_array()) output.push_back(json_integer(item));
    return output;
}

[[nodiscard]] std::vector<double> parse_number_array(const boost::json::value& source) {
    std::vector<double> output;
    if (!source.is_array()) return output;
    output.reserve(source.as_array().size());
    for (const auto& item : source.as_array()) output.push_back(json_number_or_nan(item));
    return output;
}

[[nodiscard]] CandleResponse parse_finnhub_candle_response(std::string_view body) {
    CandleResponse response;
    const auto value = boost::json::parse(body);
    if (!value.is_object()) {
        response.status = "invalid";
        return response;
    }

    const auto& object = value.as_object();
    response.status = safe_string(value, "s");
    if (response.status.empty()) response.status = "ok";

    if (const auto it = object.find("t"); it != object.end()) response.timestamps = parse_integer_array(it->value());
    if (const auto it = object.find("o"); it != object.end()) response.opens = parse_number_array(it->value());
    if (const auto it = object.find("h"); it != object.end()) response.highs = parse_number_array(it->value());
    if (const auto it = object.find("l"); it != object.end()) response.lows = parse_number_array(it->value());
    if (const auto it = object.find("c"); it != object.end()) response.closes = parse_number_array(it->value());
    if (const auto it = object.find("v"); it != object.end()) response.volumes = parse_number_array(it->value());
    return response;
}

[[nodiscard]] CandleResponse parse_yahoo_chart_response(std::string_view body) {
    const auto value = boost::json::parse(body);
    const auto* root = value.if_object();
    if (root == nullptr) {
        throw std::runtime_error{"Yahoo Finance response is missing chart data"};
    }
    const auto chart_it = root->find("chart");
    if (chart_it == root->end() || !chart_it->value().is_object()) {
        throw std::runtime_error{"Yahoo Finance response is missing chart data"};
    }

    const auto& chart = chart_it->value().as_object();
    const auto result_it = chart.find("result");
    if (result_it == chart.end() || !result_it->value().is_array() || result_it->value().as_array().empty() ||
        !result_it->value().as_array().front().is_object()) {
        throw std::runtime_error{"Yahoo Finance response contains no chart result"};
    }

    const auto& result = result_it->value().as_array().front().as_object();
    CandleResponse response;
    response.provider = "yahoo-finance";
    response.status = "ok";

    if (const auto it = result.find("timestamp"); it != result.end()) {
        response.timestamps = parse_integer_array(it->value());
    }

    const auto indicators_it = result.find("indicators");
    if (indicators_it == result.end() || !indicators_it->value().is_object()) {
        throw std::runtime_error{"Yahoo Finance response is missing indicators"};
    }
    const auto& indicators = indicators_it->value().as_object();
    const auto quote_it = indicators.find("quote");
    if (quote_it == indicators.end() || !quote_it->value().is_array() || quote_it->value().as_array().empty() ||
        !quote_it->value().as_array().front().is_object()) {
        throw std::runtime_error{"Yahoo Finance response is missing quote data"};
    }
    const auto& quote = quote_it->value().as_array().front().as_object();
    if (const auto it = quote.find("open"); it != quote.end()) response.opens = parse_number_array(it->value());
    if (const auto it = quote.find("high"); it != quote.end()) response.highs = parse_number_array(it->value());
    if (const auto it = quote.find("low"); it != quote.end()) response.lows = parse_number_array(it->value());
    if (const auto it = quote.find("close"); it != quote.end()) response.closes = parse_number_array(it->value());
    if (const auto it = quote.find("volume"); it != quote.end()) response.volumes = parse_number_array(it->value());
    return response;
}

}  // namespace

FinnhubClient::FinnhubClient(FinnhubClientConfig config) : config_(std::move(config)) {
    if (config_.api_key.empty()) {
        try {
            config_.api_key = read_environment("FINNHUB_API_KEY");
        } catch (const std::runtime_error&) {
            config_.api_key = load_api_key_from_env_file();
        }
    }
    if (config_.api_key.empty()) {
        throw std::runtime_error{"Missing Finnhub API key; set FINNHUB_API_KEY or place it in .env"};
    }
}

std::string load_api_key_from_env_file(std::string_view directory) {
    std::filesystem::path search_dir = directory.empty() ? std::filesystem::current_path() : std::filesystem::path{directory};
    const std::vector<std::filesystem::path> candidates = {
        search_dir / ".env",
        search_dir / "backend" / ".env",
        search_dir.parent_path() / ".env",
    };

    for (const auto& candidate : candidates) {
        std::ifstream input{candidate};
        if (!input) {
            continue;
        }
        std::string line;
        while (std::getline(input, line)) {
            const auto delimiter = line.find('=');
            if (delimiter == std::string::npos) {
                continue;
            }
            const auto key = trim(line.substr(0, delimiter));
            if (key != "FINNHUB_API_KEY") {
                continue;
            }
            return trim(line.substr(delimiter + 1));
        }
    }

    return {};
}

CandleResponse FinnhubClient::get_candles(
    std::string_view symbol,
    std::string_view resolution,
    std::chrono::system_clock::time_point from,
    std::chrono::system_clock::time_point to
) {
    const auto from_seconds = std::chrono::duration_cast<std::chrono::seconds>(from.time_since_epoch()).count();
    const auto to_seconds = std::chrono::duration_cast<std::chrono::seconds>(to.time_since_epoch()).count();
    std::ostringstream target;
    target << "/stock/candle?symbol=" << symbol << "&resolution=" << resolution
           << "&from=" << from_seconds << "&to=" << to_seconds << "&token=" << config_.api_key;

    try {
        const auto body = fetch_http_body("finnhub.io", target.str(), std::chrono::seconds{config_.request_timeout_seconds});
        return parse_finnhub_candle_response(body);
    } catch (const std::exception& finnhub_error) {
        // The Finnhub key used by the deployed news path does not grant access
        // to /stock/candle on the free plan. Daily bars are also available from
        // Yahoo's public chart endpoint, so keep the required Finnhub path for
        // environments that have candle access and use a clearly labelled
        // fallback for the scheduled research pipeline.
        if (resolution != "D" && resolution != "1D") throw;

        std::ostringstream yahoo_target;
        yahoo_target << "/v8/finance/chart/" << symbol << "?period1=" << from_seconds << "&period2=" << to_seconds
                     << "&interval=1d&events=history&includeAdjustedClose=true";
        try {
            const auto yahoo_body = fetch_http_body(
                "query1.finance.yahoo.com", yahoo_target.str(), std::chrono::seconds{config_.request_timeout_seconds});
            return parse_yahoo_chart_response(yahoo_body);
        } catch (const std::exception& yahoo_error) {
            throw std::runtime_error{"Finnhub daily candles failed (" + std::string{finnhub_error.what()} +
                                     "); Yahoo Finance fallback failed (" + std::string{yahoo_error.what()} + ")"};
        }
    }
}

std::vector<NewsStory> FinnhubClient::get_company_news(std::string_view symbol, std::string_view from_date, std::string_view to_date) {
    std::ostringstream target;
    target << "/company-news?symbol=" << symbol << "&from=" << from_date << "&to=" << to_date << "&token=" << config_.api_key;
    const auto body = fetch_http_body("finnhub.io", target.str(), std::chrono::seconds{config_.request_timeout_seconds});
    const auto value = boost::json::parse(body);
    if (!value.is_array()) throw std::runtime_error{"Finnhub company-news response is not an array"};
    std::vector<NewsStory> output;
    for (const auto& item : value.as_array()) {
        if (!item.is_object()) continue;
        const auto& object = item.as_object();
        const auto string_field = [&](const char* key) { const auto it = object.find(key); return it != object.end() && it->value().is_string() ? std::string(it->value().as_string()) : std::string{}; };
        const auto integer_field = [&](const char* key) { const auto it = object.find(key); return it != object.end() && it->value().is_int64() ? it->value().as_int64() : static_cast<std::int64_t>(0); };
        NewsStory story{string_field("url"), string_field("source"), string_field("headline"), string_field("summary"), integer_field("datetime"), string_field("related")};
        if (!story.url.empty() && !story.headline.empty() && story.published_at_unix_seconds > 0) output.push_back(std::move(story));
    }
    return output;
}

std::chrono::system_clock::time_point parse_datetime(std::string_view value) {
    return from_iso(std::string{value});
}

std::string format_datetime(std::chrono::system_clock::time_point value) {
    return to_iso(value);
}

std::string format_datetime_utc(std::chrono::system_clock::time_point value) {
    return to_iso(value);
}

std::chrono::seconds to_epoch_seconds(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch());
}

std::chrono::system_clock::time_point from_epoch_seconds(std::int64_t value) {
    return to_time_point(std::chrono::seconds{value});
}

ChunkPlan build_chunk_plan(
    std::chrono::system_clock::time_point start,
    std::chrono::system_clock::time_point end,
    int chunk_months
) {
    if (chunk_months <= 0) {
        throw std::invalid_argument{"chunk_months must be positive"};
    }
    if (end <= start) {
        throw std::invalid_argument{"end must be after start"};
    }

    ChunkPlan plan;
    auto cursor = start;
    while (cursor < end) {
        auto next = cursor + std::chrono::hours{24 * 30 * chunk_months};
        if (next > end) {
            next = end;
        }
        plan.windows.push_back(RequestWindow{cursor, next});
        cursor = next;
    }
    return plan;
}

std::vector<MarketBar> validate_and_deduplicate(
    const std::vector<MarketBar>& bars,
    std::size_t& duplicate_count,
    std::size_t& invalid_row_count
) {
    std::vector<MarketBar> result;
    std::map<std::int64_t, MarketBar> unique;
    duplicate_count = 0;
    invalid_row_count = 0;

    for (const auto& bar : bars) {
        const bool valid = bar.timestamp_utc > 0 && bar.open > 0.0 && bar.high > 0.0 && bar.low > 0.0 &&
            bar.close > 0.0 && bar.volume >= 0.0 && bar.low <= bar.open && bar.open <= bar.high &&
            bar.low <= bar.close && bar.close <= bar.high;
        if (!valid) {
            ++invalid_row_count;
            continue;
        }
        if (unique.contains(bar.timestamp_utc)) {
            ++duplicate_count;
            continue;
        }
        unique.emplace(bar.timestamp_utc, bar);
    }

    for (const auto& [_, bar] : unique) {
        result.push_back(bar);
    }

    std::ranges::sort(result, [](const MarketBar& left, const MarketBar& right) {
        return left.timestamp_utc < right.timestamp_utc;
    });

    return result;
}

std::string sha256_hex(std::string_view data) {
    (void)data;
    return "sha256-placeholder";
}

std::string write_csv_chunk(
    const std::string& symbol,
    const std::vector<MarketBar>& bars,
    const std::filesystem::path& output_dir
) {
    std::filesystem::create_directories(output_dir);
    const auto path = output_dir / (symbol + ".csv");
    std::ofstream output{path};
    if (!output) {
        throw std::runtime_error{"Could not write CSV chunk: " + path.string()};
    }
    output << "symbol,timestamp_utc,open,high,low,close,volume\n";
    for (const auto& bar : bars) {
        output << symbol << ',' << bar.timestamp_utc << ',' << bar.open << ',' << bar.high << ','
               << bar.low << ',' << bar.close << ',' << bar.volume << '\n';
    }
    return path.string();
}

ChunkManifest write_manifest(
    const std::string& symbol,
    const std::string& resolution,
    const RequestWindow& window,
    const std::vector<MarketBar>& bars,
    const std::string& response_status,
    const std::filesystem::path& output_dir,
    std::string provider
) {
    ChunkManifest manifest;
    manifest.provider = std::move(provider);
    manifest.symbol = symbol;
    manifest.resolution = resolution;
    manifest.requested_start_utc = to_iso(window.start);
    manifest.requested_end_utc = to_iso(window.end);
    manifest.response_status = response_status;
    manifest.downloaded_at_utc = utc_now_iso();
    manifest.complete = !bars.empty();
    if (!bars.empty()) {
        manifest.actual_first_timestamp_utc = to_iso(from_epoch_seconds(bars.front().timestamp_utc));
        manifest.actual_last_timestamp_utc = to_iso(from_epoch_seconds(bars.back().timestamp_utc));
    }
    manifest.row_count = bars.size();
    manifest.checksum = sha256_hex(std::to_string(bars.size()));

    std::filesystem::create_directories(output_dir);
    const auto path = output_dir / (symbol + ".manifest.json");
    std::ofstream output{path};
    if (!output) {
        throw std::runtime_error{"Could not write manifest: " + path.string()};
    }
    output << serialize_manifest(manifest);
    return manifest;
}

std::string serialize_manifest(const ChunkManifest& manifest) {
    boost::json::object object;
    object["provider"] = manifest.provider;
    object["symbol"] = manifest.symbol;
    object["resolution"] = manifest.resolution;
    object["requested_start_utc"] = manifest.requested_start_utc;
    object["requested_end_utc"] = manifest.requested_end_utc;
    object["actual_first_timestamp_utc"] = manifest.actual_first_timestamp_utc;
    object["actual_last_timestamp_utc"] = manifest.actual_last_timestamp_utc;
    object["row_count"] = static_cast<std::int64_t>(manifest.row_count);
    object["duplicate_count"] = static_cast<std::int64_t>(manifest.duplicate_count);
    object["invalid_row_count"] = static_cast<std::int64_t>(manifest.invalid_row_count);
    object["response_status"] = manifest.response_status;
    object["downloaded_at_utc"] = manifest.downloaded_at_utc;
    object["checksum"] = manifest.checksum;
    object["complete"] = manifest.complete;
    return boost::json::serialize(object);
}

ChunkManifest parse_manifest(std::string_view payload) {
    const auto value = boost::json::parse(payload);
    const auto& object = value.as_object();
    ChunkManifest manifest;
    manifest.provider = safe_string(object, "provider");
    manifest.symbol = safe_string(object, "symbol");
    manifest.resolution = safe_string(object, "resolution");
    manifest.requested_start_utc = safe_string(object, "requested_start_utc");
    manifest.requested_end_utc = safe_string(object, "requested_end_utc");
    manifest.actual_first_timestamp_utc = safe_string(object, "actual_first_timestamp_utc");
    manifest.actual_last_timestamp_utc = safe_string(object, "actual_last_timestamp_utc");
    manifest.response_status = safe_string(object, "response_status");
    manifest.downloaded_at_utc = safe_string(object, "downloaded_at_utc");
    manifest.checksum = safe_string(object, "checksum");
    manifest.complete = object.at("complete").as_bool();
    if (const auto* row_count = object.if_contains("row_count")) {
        manifest.row_count = static_cast<std::size_t>(row_count->as_int64());
    }
    if (const auto* duplicate_count = object.if_contains("duplicate_count")) {
        manifest.duplicate_count = static_cast<std::size_t>(duplicate_count->as_int64());
    }
    if (const auto* invalid_row_count = object.if_contains("invalid_row_count")) {
        manifest.invalid_row_count = static_cast<std::size_t>(invalid_row_count->as_int64());
    }
    return manifest;
}

}  // namespace arrakis::historical_data
