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

    const auto body = fetch_http_body("finnhub.io", target.str(), std::chrono::seconds{config_.request_timeout_seconds});
    CandleResponse response;
    const auto value = boost::json::parse(body);
    if (!value.is_object()) {
        response.status = "invalid";
        return response;
    }

    const auto& object = value.as_object();
    response.status = safe_string(object, "s");
    if (response.status.empty()) {
        response.status = "ok";
    }

    const auto parse_array = [](const boost::json::value& source, std::vector<std::int64_t>& target) {
        if (!source.is_array()) {
            return;
        }
        for (const auto& item : source.as_array()) {
            target.push_back(static_cast<std::int64_t>(item.as_int64()));
        }
    };
    const auto parse_double_array = [](const boost::json::value& source, std::vector<double>& target) {
        if (!source.is_array()) {
            return;
        }
        for (const auto& item : source.as_array()) {
            target.push_back(item.as_double());
        }
    };

    if (const auto it = object.find("t"); it != object.end()) {
        parse_array(it->value(), response.timestamps);
    }
    if (const auto it = object.find("o"); it != object.end()) {
        parse_double_array(it->value(), response.opens);
    }
    if (const auto it = object.find("h"); it != object.end()) {
        parse_double_array(it->value(), response.highs);
    }
    if (const auto it = object.find("l"); it != object.end()) {
        parse_double_array(it->value(), response.lows);
    }
    if (const auto it = object.find("c"); it != object.end()) {
        parse_double_array(it->value(), response.closes);
    }
    if (const auto it = object.find("v"); it != object.end()) {
        parse_double_array(it->value(), response.volumes);
    }
    return response;
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
    const std::filesystem::path& output_dir
) {
    ChunkManifest manifest;
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
