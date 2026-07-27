#include "arrakis/bar_aggregator/aggregation.hpp"
#include "arrakis/streaming/kafka.hpp"
#include "arrakis/serialization/serialization.hpp"
#include "arrakis/runtime/config.hpp"
#include "arrakis/runtime/metrics.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {
volatile std::sig_atomic_t running = 1;
void stop(int) { running = 0; }
std::string env(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value == nullptr ? fallback : value;
}
std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}

int main() {
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    try {
        const auto brokers = env("KAFKA_BOOTSTRAP_SERVERS", "localhost:9092");
        const auto config_path = env("ARRAKIS_BAR_CONFIG", "config/bar_aggregator.json");
        const auto config = arrakis::runtime::load_bar_config(config_path);
        arrakis::runtime::Metrics metrics;
        arrakis::runtime::MetricsServer metrics_server(metrics, config.metrics_port);
        arrakis::streaming::KafkaConsumer consumer(brokers, config.consumer_group, config.input_topic);
        arrakis::streaming::KafkaProducer producer(brokers, "bar-aggregator-v1");
        arrakis::bar_aggregator::BarAggregator aggregator(
            config.bar_interval_seconds,
            config.allowed_lateness_seconds,
            config.deduplication_window_minutes,
            std::chrono::seconds(1));
        std::unordered_map<std::string, std::uint64_t> maximum_event_time;

        while (running != 0) {
            const auto record = consumer.poll(std::chrono::milliseconds(250));
            if (!record) {
                producer.poll_events(std::chrono::milliseconds(0));
                continue;
            }
            try {
                const auto trade = arrakis::streaming::deserialize_trade(record->payload);
                metrics.increment("trade_events_consumed_total");
                maximum_event_time[trade.symbol] = std::max(
                    maximum_event_time[trade.symbol], trade.source_timestamp_unix_ms);
                static_cast<void>(aggregator.onTrade(trade));

                const auto lateness_ms = static_cast<std::uint64_t>(config.allowed_lateness_seconds) * 1000U;
                const auto watermark = maximum_event_time[trade.symbol] > lateness_ms
                    ? maximum_event_time[trade.symbol] - lateness_ms : 0U;
                bool output_pending = false;
                for (const auto& bar : aggregator.advanceWatermarkForSymbol(trade.symbol, watermark)) {
                    producer.publish(config.output_topic, bar.symbol, arrakis::streaming::serialize_bar(bar));
                    output_pending = true;
                    metrics.increment("bars_published_total");
                    metrics.increment("bars_finalized_total");
                }
                for (const auto& late : aggregator.drainLateTrades()) {
                    producer.publish(config.late_trade_topic, late.trade.symbol,
                                     arrakis::streaming::serialize_late_trade(late.trade, late.reason));
                    output_pending = true;
                    metrics.increment("late_trades_total");
                }
                if (output_pending) producer.flush(std::chrono::seconds(5));
                metrics.set("kafka_delivery_failures_total",
                            static_cast<std::int64_t>(producer.delivery_failures()));
                producer.poll_events(std::chrono::milliseconds(0));
                consumer.commit(*record);
            } catch (const std::exception& error) {
                const auto dead_letter = arrakis::streaming::serialize_dead_letter(
                    "bar-aggregator", record->payload, "invalid_trade_event", error.what(), now_ms());
                producer.publish(config.dead_letter_topic, record->key, dead_letter);
                producer.flush(std::chrono::seconds(5));
                producer.poll_events(std::chrono::milliseconds(0));
                consumer.commit(*record);
                metrics.increment("trade_events_rejected_total");
                std::cerr << "{\"service\":\"bar-aggregator\",\"error\":\""
                          << error.what() << "\"}\n";
            }
        }
        producer.flush(std::chrono::seconds(5));
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "{\"service\":\"bar-aggregator\",\"fatal\":\""
                  << error.what() << "\"}\n";
        return EXIT_FAILURE;
    }
}
