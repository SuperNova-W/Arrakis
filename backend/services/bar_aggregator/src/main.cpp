#include "arrakis/bar_aggregator/aggregation.hpp"
#include "arrakis/database/postgres.hpp"
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
        arrakis::database::PostgresPool database(arrakis::database::database_config_from_environment());
        arrakis::bar_aggregator::BarAggregator aggregator(
            config.bar_interval_seconds,
            config.allowed_lateness_seconds,
            config.deduplication_window_minutes,
            std::chrono::seconds(1));
        std::unordered_map<std::string, std::uint64_t> maximum_event_time;

        while (running != 0) {
            const auto record = consumer.poll(std::chrono::milliseconds(250));
            if (!record) {
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
                const auto one_minute = aggregator.advanceWatermarkForSymbol(trade.symbol, watermark);
                const auto five_minute = aggregator.drainCompletedFiveMinuteBars();
                if (!one_minute.empty() || !five_minute.empty()) {
                    database.persist_bars(one_minute, five_minute);
                    metrics.increment("database_bar_insert_success_total", one_minute.size() + five_minute.size());
                    metrics.increment("one_minute_bars_finalized_total", one_minute.size());
                    metrics.increment("five_minute_bars_finalized_total", five_minute.size());
                }
                const auto late_trades = aggregator.drainLateTrades();
                metrics.increment("late_trades_total", late_trades.size());
                consumer.commit(*record);
            } catch (const std::exception& error) {
                consumer.commit(*record);
                metrics.increment("trade_events_rejected_total");
                std::cerr << "{\"service\":\"bar-aggregator\",\"event\":\"rejected\",\"error\":\""
                          << error.what() << "\"}\n";
            }
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "{\"service\":\"bar-aggregator\",\"fatal\":\""
                  << error.what() << "\"}\n";
        return EXIT_FAILURE;
    }
}
