#include "arrakis/market/finnhub_message.hpp"
#include "arrakis/market/normalization.hpp"
#include "arrakis/streaming/kafka.hpp"
#include "arrakis/serialization/serialization.hpp"
#include "arrakis/runtime/config.hpp"
#include "arrakis/runtime/metrics.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/json.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <charconv>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <thread>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <random>
#include <atomic>
#include <unordered_set>

namespace { volatile std::sig_atomic_t g_running = 1; void stop_signal(int) { g_running = 0; } }

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

namespace {

struct Options {
    std::vector<std::string> symbols;
    std::size_t max_events{};
    std::string fixture_path;
};

[[nodiscard]] std::string trim_upper(std::string_view input) {
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())) != 0) {
        input.remove_prefix(1);
    }
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())) != 0) {
        input.remove_suffix(1);
    }
    if (input.empty()) {
        throw std::runtime_error("--symbols cannot contain empty values");
    }

    std::string result(input);
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return result;
}

[[nodiscard]] std::vector<std::string> split_symbols(std::string_view input) {
    std::vector<std::string> symbols;
    std::size_t start = 0;
    while (start <= input.size()) {
        const auto end = input.find(',', start);
        const auto token = input.substr(start, end == std::string_view::npos ? input.size() - start
                                                                            : end - start);
        symbols.push_back(trim_upper(token));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return symbols;
}

[[nodiscard]] std::size_t parse_size(std::string_view value, std::string_view option) {
    std::size_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error(std::string(option) + " must be a non-negative integer");
    }
    return result;
}

void print_usage(std::ostream& output) {
    output << "Usage: market-ingestion [--symbols IWM,SPY] [--max-events N] [--fixture FILE]\n"
              "\n"
              "Environment:\n"
              "  FINNHUB_API_KEY      Finnhub API token (live mode only)\n"
              "\n"
              "Defaults to the configured ETF universe. A max-events value of 0 streams until interrupted.\n"
              "Fixture files are JSONL Finnhub trade frames and use the same validation and Kafka path as live data.\n";
}

[[nodiscard]] Options parse_options(int argc, char* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            print_usage(std::cout);
            std::exit(EXIT_SUCCESS);
        }
        if (index + 1 >= argc) {
            throw std::runtime_error("Missing value for option: " + std::string(argument));
        }
        const std::string_view value = argv[++index];
        if (argument == "--symbols") {
            options.symbols = split_symbols(value);
        } else if (argument == "--max-events") {
            options.max_events = parse_size(value, argument);
        } else if (argument == "--fixture") {
            options.fixture_path = value;
        } else {
            throw std::runtime_error("Unknown option: " + std::string(argument));
        }
    }
    return options;
}

[[nodiscard]] std::string required_environment_variable(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || std::string_view(value).empty()) {
        throw std::runtime_error(
            std::string("Missing environment variable ") + name +
            ". Set it locally; never commit Finnhub credentials."
        );
    }
    return value;
}

template <typename WebSocket>
void write_json(WebSocket& stream, const boost::json::object& object) {
    const auto payload = boost::json::serialize(object);
    stream.write(asio::buffer(payload));
}

template <typename WebSocket>
[[nodiscard]] arrakis::market::FinnhubFrame read_frame(WebSocket& stream) {
    beast::flat_buffer buffer;
    stream.read(buffer);
    return arrakis::market::parse_finnhub_frame(beast::buffers_to_string(buffer.data()));
}

void run(const Options& options) {
    const auto config_path = [] { const char* value = std::getenv("ARRAKIS_INGESTION_CONFIG"); return value == nullptr ? std::string("config/ingestion.json") : std::string(value); }();
    const auto config = arrakis::runtime::load_ingestion_config(config_path);
    arrakis::runtime::Metrics metrics;
    arrakis::runtime::MetricsServer metrics_server(metrics, config.metrics_port);
    const auto brokers = [] { const char* value = std::getenv("KAFKA_BOOTSTRAP_SERVERS"); return value == nullptr ? std::string("localhost:9092") : std::string(value); }();
    arrakis::streaming::KafkaProducer producer(brokers, "finnhub-ingestion-v1");
    const auto& symbols = options.symbols.empty() ? config.symbols : options.symbols;
    const std::unordered_set<std::string> allowed_symbols(symbols.begin(), symbols.end());
    std::size_t event_count = 0;

    const auto process_frame = [&](const arrakis::market::FinnhubFrame& frame) {
        for (const auto& control : frame.controls) {
            if (control.kind == arrakis::market::ControlKind::error) {
                throw std::runtime_error("Finnhub error: " + control.message);
            }
            if (!control.message.empty()) std::cerr << "Finnhub: " << control.message << '\n';
        }
        for (std::size_t sequence = 0; sequence < frame.trades.size(); ++sequence) {
            if (options.max_events != 0 && event_count >= options.max_events) break;
            const auto& trade = frame.trades[sequence];
            try {
                auto normalized = arrakis::market::normalize_trade(trade, "finnhub", sequence);
                if (!allowed_symbols.contains(normalized.symbol)) throw std::runtime_error("symbol is not in configured ETF universe");
                normalized.received_timestamp_unix_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
                const auto payload = arrakis::streaming::serialize_trade(normalized);
                producer.publish(config.raw_trade_topic, normalized.symbol, payload);
                producer.poll_events(std::chrono::milliseconds(0));
                metrics.set("kafka_delivery_failures_total", static_cast<std::int64_t>(producer.delivery_failures()));
                metrics.increment("finnhub_trade_events_total");
                metrics.increment("kafka_trade_publish_success_total");
                std::cout << "{\"service\":\"market-ingestion\",\"symbol\":\"" << normalized.symbol << "\",\"event_id\":\"" << normalized.event_id << "\"}\n";
            } catch (const std::exception& error) {
                metrics.increment("finnhub_malformed_messages_total");
                metrics.increment("kafka_trade_publish_failure_total");
                std::cerr << "{\"service\":\"market-ingestion\",\"error_code\":\"invalid_trade\",\"error\":\"" << error.what() << "\"}\n";
            }
            ++event_count;
        }
    };

    if (!options.fixture_path.empty()) {
        std::ifstream fixture(options.fixture_path);
        if (!fixture) throw std::runtime_error("Unable to open fixture: " + options.fixture_path);
        std::string line;
        while (g_running != 0 && std::getline(fixture, line)) {
            if (line.empty()) continue;
            try {
                process_frame(arrakis::market::parse_finnhub_frame(line));
                metrics.increment("finnhub_messages_received_total");
            } catch (const std::exception& error) {
                metrics.increment("finnhub_malformed_messages_total");
                std::cerr << "{\"service\":\"market-ingestion\",\"error_code\":\"malformed_finnhub_message\",\"error\":\"" << error.what() << "\"}\n";
            }
            if (options.max_events != 0 && event_count >= options.max_events) break;
        }
        producer.flush(std::chrono::seconds(5));
        return;
    }

    const auto api_key = required_environment_variable("FINNHUB_API_KEY");
    const auto host = config.websocket_host;
    const auto target = config.websocket_path + "?token=" + api_key;

    asio::io_context io_context;
    ssl::context tls_context(ssl::context::tls_client);
    tls_context.set_default_verify_paths();
    tls_context.set_verify_mode(ssl::verify_peer);

    tcp::resolver resolver(io_context);
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> stream(io_context, tls_context);

    if (!SSL_set_tlsext_host_name(stream.next_layer().native_handle(), host.data())) {
        const auto error_code = static_cast<int>(::ERR_get_error());
        throw beast::system_error(
            beast::error_code(error_code, asio::error::get_ssl_category()),
            "Unable to configure TLS server name"
        );
    }
    stream.next_layer().set_verify_callback(ssl::host_name_verification(std::string(host)));

    const auto endpoints = resolver.resolve(host, config.websocket_port);
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(stream).connect(endpoints);
    stream.next_layer().handshake(ssl::stream_base::client);

    beast::get_lowest_layer(stream).expires_never();
    stream.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    stream.set_option(websocket::stream_base::decorator([](websocket::request_type& request) {
        request.set(http::field::user_agent, "Arrakis/0.1 Boost.Beast");
        request.set(http::field::content_type, "application/json");
    }));
    stream.handshake(std::string(host), target);
    stream.text(true);

    for (const auto& symbol : symbols) {
        write_json(stream, boost::json::object{
            {"type", "subscribe"},
            {"symbol", symbol},
        });
    }

    while (g_running != 0 && (options.max_events == 0 || event_count < options.max_events)) {
        beast::flat_buffer input_buffer;
        stream.read(input_buffer);
        const auto raw_payload = beast::buffers_to_string(input_buffer.data());
        arrakis::market::FinnhubFrame frame;
        try { frame = arrakis::market::parse_finnhub_frame(raw_payload); metrics.increment("finnhub_messages_received_total"); }
        catch (const std::exception& error) {
            metrics.increment("finnhub_malformed_messages_total");
            std::cerr << "{\"service\":\"market-ingestion\",\"error_code\":\"malformed_finnhub_message\",\"error\":\"" << error.what() << "\"}\n";
            continue;
        }
        process_frame(frame);
    }

    producer.flush(std::chrono::seconds(5));
    stream.close(websocket::close_code::normal);
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        std::signal(SIGINT, stop_signal); std::signal(SIGTERM, stop_signal);
        const auto options = parse_options(argc, argv);
        const auto config_path = [] { const char* value = std::getenv("ARRAKIS_INGESTION_CONFIG"); return value == nullptr ? std::string("config/ingestion.json") : std::string(value); }();
        const auto reconnect_config = arrakis::runtime::load_ingestion_config(config_path);
        std::uint32_t delay_ms = reconnect_config.initial_reconnect_delay_ms;
        std::mt19937 generator(std::random_device{}());
        while (g_running != 0) {
            try {
                run(options);
                return EXIT_SUCCESS;
            } catch (const std::exception& exception) {
                std::cerr << "{\"service\":\"market-ingestion\",\"error\":\"" << exception.what() << "\",\"retry_ms\":" << delay_ms << "}\n";
                const auto jitter_percent = static_cast<int>(reconnect_config.reconnect_jitter_percent);
                std::uniform_int_distribution<int> jitter(-jitter_percent, jitter_percent);
                const auto jittered = std::max<std::uint32_t>(100U, delay_ms * static_cast<std::uint32_t>(100 + jitter(generator)) / 100U);
                std::this_thread::sleep_for(std::chrono::milliseconds(jittered));
                delay_ms = std::min(delay_ms * 2U, reconnect_config.maximum_reconnect_delay_ms);
            }
        }
    } catch (const std::exception& exception) {
        std::cerr << "market-ingestion: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
