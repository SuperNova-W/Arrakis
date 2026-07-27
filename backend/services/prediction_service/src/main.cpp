#include "arrakis/prediction/prediction_service.hpp"
#include "arrakis/serialization/serialization.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>

namespace { volatile std::sig_atomic_t running = 1; void stop(int) { running = 0; } const char* env(const char* key, const char* fallback) { const char* value = std::getenv(key); return value == nullptr ? fallback : value; } }
int main() {
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
    try {
        const auto config = arrakis::prediction::load_config(env("ARRAKIS_PREDICTION_CONFIG", "config/prediction_service.json"));
        arrakis::runtime::Metrics metrics; arrakis::runtime::MetricsServer metrics_server(metrics, config.metrics_port);
        arrakis::prediction::PredictionService service(config, metrics);
        arrakis::streaming::KafkaConsumer consumer(env("KAFKA_BOOTSTRAP_SERVERS", "localhost:9092"), config.consumer_group, config.input_topic);
        arrakis::streaming::KafkaProducer producer(env("KAFKA_BOOTSTRAP_SERVERS", "localhost:9092"), "xgb-prediction-v1");
        while (running != 0) {
            const auto record = consumer.poll(std::chrono::milliseconds(250)); if (!record) { producer.poll_events(std::chrono::milliseconds(0)); continue; }
            try { const auto prediction = service.predict(record->payload); producer.publish(config.output_topic, record->key, prediction); producer.poll_events(std::chrono::milliseconds(0)); consumer.commit(*record); }
            catch (const std::exception& error) { metrics.increment("prediction_events_rejected_total"); const auto dead = arrakis::streaming::serialize_dead_letter("prediction-service", record->payload, "prediction_rejected", error.what(), std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); producer.publish(config.dead_letter_topic, record->key, dead); producer.flush(std::chrono::seconds(5)); consumer.commit(*record); std::cerr << "{\"service\":\"prediction-service\",\"event\":\"rejected\",\"error\":\"" << error.what() << "\"}\n"; }
        }
        producer.flush(std::chrono::seconds(5)); return EXIT_SUCCESS;
    } catch (const std::exception& error) { std::cerr << "{\"service\":\"prediction-service\",\"event\":\"fatal\",\"error\":\"" << error.what() << "\"}\n"; return EXIT_FAILURE; }
}
