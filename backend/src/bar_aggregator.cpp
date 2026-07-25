#include "arrakis/market/normalization.hpp"
#include "arrakis/streaming/kafka.hpp"
#include "arrakis/streaming/serialization.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

namespace { volatile std::sig_atomic_t running = 1; void stop(int) { running = 0; }
std::string env(const char* name, const char* fallback) { const char* value = std::getenv(name); return value == nullptr ? fallback : value; }
}

int main() {
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
    try {
        const auto brokers = env("KAFKA_BOOTSTRAP_SERVERS", "localhost:9092");
        arrakis::streaming::KafkaConsumer consumer(brokers, "bar-aggregation-v1", "market.raw.trades");
        arrakis::streaming::KafkaProducer producer(brokers, "bar-aggregator-v1");
        arrakis::market::BarAggregator aggregator(60, 5, 10, std::chrono::seconds(1));
        std::uint64_t maximum_event_time = 0;
        while (running != 0) {
            const auto record = consumer.poll(std::chrono::milliseconds(250));
            if (!record) { producer.poll_events(std::chrono::milliseconds(0)); continue; }
            try {
                const auto trade = arrakis::streaming::deserialize_trade(record->payload);
                maximum_event_time = std::max(maximum_event_time, trade.source_timestamp_unix_ms);
                static_cast<void>(aggregator.onTrade(trade));
                const auto watermark = maximum_event_time > 5000U ? maximum_event_time - 5000U : 0U;
                for (const auto& bar : aggregator.advanceWatermark(watermark)) {
                    const auto payload = arrakis::streaming::serialize_bar(bar);
                    producer.publish("market.bars.1m", bar.symbol, payload);
                }
                producer.poll_events(std::chrono::milliseconds(0));
                consumer.commit(*record);
            } catch (const std::exception& error) {
                std::cerr << "{\"service\":\"bar-aggregator\",\"error\":\"" << error.what() << "\"}\n";
                // A malformed record is not committed here. It remains replayable and should be
                // routed to dead-letter.events by the production error-policy adapter.
            }
        }
        producer.flush(std::chrono::seconds(5));
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "{\"service\":\"bar-aggregator\",\"fatal\":\"" << error.what() << "\"}\n";
        return EXIT_FAILURE;
    }
}
