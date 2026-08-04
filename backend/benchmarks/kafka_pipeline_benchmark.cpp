#include "arrakis/market/normalization.hpp"
#include "arrakis/serialization/serialization.hpp"
#include "arrakis/streaming/kafka.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct Options {
    std::string brokers{"localhost:9092"};
    std::string topic{"arrakis.benchmark.trades"};
    std::size_t events{100000};
    std::size_t warmup{5000};
    bool commit_offsets{true};
};

std::uint64_t epoch_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto value = [&](const char* name) -> std::string {
            if (++index >= argc) throw std::invalid_argument(std::string("missing value for ") + name);
            return argv[index];
        };
        if (argument == "--brokers") options.brokers = value("--brokers");
        else if (argument == "--topic") options.topic = value("--topic");
        else if (argument == "--events") options.events = std::stoull(value("--events"));
        else if (argument == "--warmup") options.warmup = std::stoull(value("--warmup"));
        else if (argument == "--no-commit") options.commit_offsets = false;
        else throw std::invalid_argument("unknown argument: " + argument);
    }
    if (options.events == 0) throw std::invalid_argument("events must be positive");
    return options;
}

double percentile(const std::vector<double>& sorted, double quantile) {
    const double position = quantile * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    const double fraction = position - static_cast<double>(lower);
    return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
}

arrakis::market::NormalizedTrade trade_for(std::size_t sequence) {
    static constexpr const char* symbols[] = {
        "XLC", "XLY", "XLP", "XLE", "XLF", "XLV", "XLI", "XLB", "XLRE",
        "XLK", "XLU", "SPY", "QQQ", "IWM", "TLT", "HYG", "GLD", "USO"
    };
    arrakis::market::NormalizedTrade trade;
    trade.symbol = symbols[sequence % std::size(symbols)];
    trade.event_id = "benchmark-" + trade.symbol + "-" + std::to_string(sequence);
    trade.correlation_id = "benchmark-run";
    trade.source = "arrakis-benchmark";
    trade.price = 630.0 + static_cast<double>(sequence % 100) / 100.0;
    trade.volume = 100.0 + static_cast<double>(sequence % 900);
    trade.source_timestamp_unix_ms = epoch_ms();
    trade.received_timestamp_unix_ms = trade.source_timestamp_unix_ms;
    trade.conditions = {"benchmark", "regular-sale"};
    return trade;
}
}

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const std::string group = "arrakis-benchmark-" + std::to_string(epoch_ms());
        arrakis::streaming::KafkaConsumer consumer(options.brokers, group, options.topic);
        arrakis::streaming::KafkaProducer producer(options.brokers, "arrakis-kafka-benchmark");

        std::vector<double> latency_ms;
        latency_ms.reserve(options.events);
        std::atomic_bool consumer_ready{false};
        std::atomic_size_t received_count{0};
        std::exception_ptr consumer_error;
        std::size_t payload_bytes = 0;
        Clock::time_point first_publish;
        Clock::time_point last_receive;

        std::thread consumer_thread([&] {
            try {
                consumer_ready = true;
                std::size_t received = 0;
                const auto deadline = Clock::now() + std::chrono::minutes(10);
                const auto total_events = options.warmup + options.events;
                while (received < total_events) {
                    if (Clock::now() >= deadline) throw std::runtime_error("benchmark timed out");
                    const auto record = consumer.poll(std::chrono::milliseconds(100));
                    if (!record) continue;
                    const auto trade = arrakis::streaming::deserialize_trade(record->payload);
                    const auto arrival_ms = epoch_ms();
                    if (received >= options.warmup) {
                        latency_ms.push_back(static_cast<double>(arrival_ms - trade.received_timestamp_unix_ms));
                    }
                    if (options.commit_offsets) consumer.commit(*record);
                    ++received;
                    received_count = received;
                }
                last_receive = Clock::now();
            } catch (...) {
                consumer_error = std::current_exception();
            }
        });

        while (!consumer_ready) std::this_thread::yield();
        for (std::size_t sequence = 0; sequence < options.warmup; ++sequence) {
            auto trade = trade_for(sequence);
            auto payload = arrakis::streaming::serialize_trade(trade);
            producer.publish(options.topic, trade.symbol, payload);
            producer.poll_events(std::chrono::milliseconds(0));
        }
        producer.flush(std::chrono::seconds(30));
        const auto warmup_deadline = Clock::now() + std::chrono::minutes(2);
        while (received_count < options.warmup) {
            if (Clock::now() >= warmup_deadline) throw std::runtime_error("warmup timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        first_publish = Clock::now();
        for (std::size_t sequence = 0; sequence < options.events; ++sequence) {
            auto trade = trade_for(options.warmup + sequence);
            auto payload = arrakis::streaming::serialize_trade(trade);
            payload_bytes += payload.size();
            producer.publish(options.topic, trade.symbol, payload);
            producer.poll_events(std::chrono::milliseconds(0));
        }
        producer.flush(std::chrono::seconds(30));
        consumer_thread.join();
        if (consumer_error) std::rethrow_exception(consumer_error);
        if (producer.delivery_failures() != 0) throw std::runtime_error("Kafka delivery failures occurred");

        std::sort(latency_ms.begin(), latency_ms.end());
        const double duration_seconds = std::chrono::duration<double>(last_receive - first_publish).count();
        const double mean_latency = std::accumulate(latency_ms.begin(), latency_ms.end(), 0.0) /
            static_cast<double>(latency_ms.size());
        const double average_payload = static_cast<double>(payload_bytes) / static_cast<double>(options.events);

        std::cout
            << "{\n"
            << "  \"events\": " << options.events << ",\n"
            << "  \"warmup_events\": " << options.warmup << ",\n"
            << "  \"measured_events\": " << latency_ms.size() << ",\n"
            << "  \"duration_seconds\": " << duration_seconds << ",\n"
            << "  \"throughput_events_per_second\": " << static_cast<double>(options.events) / duration_seconds << ",\n"
            << "  \"latency_mean_ms\": " << mean_latency << ",\n"
            << "  \"latency_p50_ms\": " << percentile(latency_ms, 0.50) << ",\n"
            << "  \"latency_p95_ms\": " << percentile(latency_ms, 0.95) << ",\n"
            << "  \"latency_p99_ms\": " << percentile(latency_ms, 0.99) << ",\n"
            << "  \"average_payload_bytes\": " << average_payload << ",\n"
            << "  \"manual_offset_commit_per_event\": " << (options.commit_offsets ? "true" : "false") << ",\n"
            << "  \"delivery_failures\": " << producer.delivery_failures() << "\n"
            << "}\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-kafka-benchmark: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
