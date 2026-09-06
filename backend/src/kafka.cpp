#include "arrakis/streaming/kafka.hpp"

#include <librdkafka/rdkafka.h>

#include <stdexcept>
#include <string>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <memory>

namespace arrakis::streaming {
namespace {
void delivery_report(rd_kafka_t*, const rd_kafka_message_t* message, void* opaque) {
    if (message->err != RD_KAFKA_RESP_ERR_NO_ERROR) {
        if (opaque != nullptr) ++*static_cast<std::atomic<std::uint64_t>*>(opaque);
        std::fprintf(stderr, "{\"service\":\"kafka-producer\",\"event\":\"delivery_failure\",\"error\":\"%s\"}\n", rd_kafka_err2str(message->err));
    }
}
void check(rd_kafka_resp_err_t error, const char* action) {
    if (error != RD_KAFKA_RESP_ERR_NO_ERROR) throw std::runtime_error(std::string(action) + ": " + rd_kafka_err2str(error));
}
rd_kafka_conf_t* config(std::string_view brokers, std::string_view client, bool producer) {
    rd_kafka_conf_t* conf = rd_kafka_conf_new();
    char error[512]{};
    auto set = [&](const char* key, const char* value) { if (rd_kafka_conf_set(conf, key, value, error, sizeof(error)) != RD_KAFKA_CONF_OK) throw std::runtime_error(std::string("Kafka config ") + key + ": " + error); };
    set("bootstrap.servers", std::string(brokers).c_str()); set("client.id", std::string(client).c_str());
    if (producer) { set("acks", "all"); set("enable.idempotence", "true"); set("compression.type", "zstd"); set("delivery.timeout.ms", "30000"); set("retries", "10"); rd_kafka_conf_set_dr_msg_cb(conf, delivery_report); }
    else {
        set("enable.auto.commit", "false");
        set("auto.offset.reset", "earliest");
        // A scheduled batch can legitimately have no eligible news. Allow the
        // consumer to subscribe to the broker-created raw topic in that case;
        // the enricher's one-shot mode will persist an empty feature vector.
        set("allow.auto.create.topics", "true");
    }
    return conf;
}
}

struct KafkaProducer::Impl { rd_kafka_t* handle{}; std::atomic<std::uint64_t> delivery_failures{}; };
KafkaProducer::KafkaProducer(std::string_view brokers, std::string_view client_id) : impl_(std::make_unique<Impl>()) {
    char error[512]{}; auto* conf = config(brokers, client_id, true); rd_kafka_conf_set_opaque(conf, &impl_->delivery_failures); impl_->handle = rd_kafka_new(RD_KAFKA_PRODUCER, conf, error, sizeof(error));
    if (impl_->handle == nullptr) throw std::runtime_error(std::string("Kafka producer: ") + error);
}
KafkaProducer::~KafkaProducer() { if (impl_ && impl_->handle) { rd_kafka_flush(impl_->handle, 5000); rd_kafka_destroy(impl_->handle); } }
void KafkaProducer::publish(std::string_view topic, std::string_view key, std::span<const std::byte> payload) {
    const std::string topic_name(topic);
    rd_kafka_topic_t* kafka_topic = rd_kafka_topic_new(impl_->handle, topic_name.c_str(), nullptr);
    if (kafka_topic == nullptr) throw std::runtime_error("Kafka topic creation failed");
    const auto error = rd_kafka_produce(kafka_topic, RD_KAFKA_PARTITION_UA,
        RD_KAFKA_MSG_F_COPY, const_cast<void*>(static_cast<const void*>(payload.data())), payload.size(),
        key.data(), key.size(), nullptr);
    rd_kafka_topic_destroy(kafka_topic);
    if (error != 0) throw std::runtime_error(std::string("Kafka publish: ") + rd_kafka_err2str(rd_kafka_last_error()));
}
void KafkaProducer::ensure_topic(std::string_view topic, int partitions, int replication_factor) {
    char error[512]{};
    const std::string topic_name(topic);
    std::unique_ptr<rd_kafka_NewTopic_t, decltype(&rd_kafka_NewTopic_destroy)> new_topic(
        rd_kafka_NewTopic_new(topic_name.c_str(), partitions, replication_factor, error, sizeof(error)),
        &rd_kafka_NewTopic_destroy);
    if (!new_topic) throw std::runtime_error(std::string("Kafka topic definition: ") + error);

    std::unique_ptr<rd_kafka_AdminOptions_t, decltype(&rd_kafka_AdminOptions_destroy)> options(
        rd_kafka_AdminOptions_new(impl_->handle, RD_KAFKA_ADMIN_OP_CREATETOPICS),
        &rd_kafka_AdminOptions_destroy);
    if (!options) throw std::runtime_error("Kafka admin options allocation failed");
    check(rd_kafka_AdminOptions_set_request_timeout(options.get(), 45000, error, sizeof(error)), "Kafka admin request timeout");
    check(rd_kafka_AdminOptions_set_operation_timeout(options.get(), 30000, error, sizeof(error)), "Kafka admin operation timeout");

    std::unique_ptr<rd_kafka_queue_t, decltype(&rd_kafka_queue_destroy)> queue(rd_kafka_queue_new(impl_->handle), &rd_kafka_queue_destroy);
    if (!queue) throw std::runtime_error("Kafka admin queue allocation failed");
    rd_kafka_NewTopic_t* topics[] = {new_topic.get()};
    rd_kafka_CreateTopics(impl_->handle, topics, 1, options.get(), queue.get());
    std::unique_ptr<rd_kafka_event_t, decltype(&rd_kafka_event_destroy)> event(rd_kafka_queue_poll(queue.get(), 45000), &rd_kafka_event_destroy);
    if (!event) throw std::runtime_error("Kafka topic creation timed out");
    check(rd_kafka_event_error(event.get()), "Kafka topic creation request");
    if (rd_kafka_event_type(event.get()) != RD_KAFKA_EVENT_CREATETOPICS_RESULT) throw std::runtime_error("Kafka topic creation returned an unexpected event");
    size_t result_count = 0;
    const auto* results = rd_kafka_CreateTopics_result_topics(rd_kafka_event_CreateTopics_result(event.get()), &result_count);
    if (results == nullptr || result_count != 1) throw std::runtime_error("Kafka topic creation returned no topic result");
    const auto result_error = rd_kafka_topic_result_error(results[0]);
    if (result_error != RD_KAFKA_RESP_ERR_NO_ERROR && result_error != RD_KAFKA_RESP_ERR_TOPIC_ALREADY_EXISTS) {
        throw std::runtime_error(std::string("Kafka topic creation: ") + rd_kafka_err2str(result_error));
    }
}
void KafkaProducer::poll_events(std::chrono::milliseconds timeout) { rd_kafka_poll(impl_->handle, static_cast<int>(timeout.count())); }
void KafkaProducer::flush(std::chrono::milliseconds timeout) { check(rd_kafka_flush(impl_->handle, static_cast<int>(timeout.count())), "Kafka flush"); }
bool KafkaProducer::usable() const noexcept { return impl_ && impl_->handle; }
std::uint64_t KafkaProducer::delivery_failures() const noexcept { return impl_ == nullptr ? 0U : impl_->delivery_failures.load(); }

struct KafkaConsumer::Impl { rd_kafka_t* handle{}; std::string topic; };
KafkaConsumer::KafkaConsumer(std::string_view brokers, std::string_view group, std::string_view topic) : impl_(std::make_unique<Impl>()) {
    auto* conf = config(brokers, group, false); char error[512]{};
    if (rd_kafka_conf_set(conf, "group.id", std::string(group).c_str(), error, sizeof(error)) != RD_KAFKA_CONF_OK) throw std::runtime_error(error);
    impl_->handle = rd_kafka_new(RD_KAFKA_CONSUMER, conf, error, sizeof(error)); if (!impl_->handle) throw std::runtime_error(error);
    rd_kafka_poll_set_consumer(impl_->handle); impl_->topic = topic;
    auto* topics = rd_kafka_topic_partition_list_new(1); rd_kafka_topic_partition_list_add(topics, impl_->topic.c_str(), RD_KAFKA_PARTITION_UA);
    check(rd_kafka_subscribe(impl_->handle, topics), "Kafka subscribe"); rd_kafka_topic_partition_list_destroy(topics);
}
KafkaConsumer::~KafkaConsumer() { if (impl_ && impl_->handle) { rd_kafka_consumer_close(impl_->handle); rd_kafka_destroy(impl_->handle); } }
std::optional<KafkaRecord> KafkaConsumer::poll(std::chrono::milliseconds timeout) {
    rd_kafka_message_t* message = rd_kafka_consumer_poll(impl_->handle, static_cast<int>(timeout.count())); if (!message) return std::nullopt;
    if (message->err) { const std::string error = rd_kafka_message_errstr(message); const auto code = message->err; rd_kafka_message_destroy(message); if (code == RD_KAFKA_RESP_ERR__TIMED_OUT) return std::nullopt; throw std::runtime_error(error); }
    const std::string key = message->key == nullptr ? std::string{} : std::string(static_cast<const char*>(message->key), message->key_len);
    KafkaRecord result{impl_->topic, key, {}, message->partition, message->offset};
    result.payload.resize(message->len); std::memcpy(result.payload.data(), message->payload, message->len); rd_kafka_message_destroy(message); return result;
}
void KafkaConsumer::commit(const KafkaRecord& record) { auto* list = rd_kafka_topic_partition_list_new(1); auto* item = rd_kafka_topic_partition_list_add(list, record.topic.c_str(), record.partition); item->offset = record.offset + 1; check(rd_kafka_commit(impl_->handle, list, 1), "Kafka commit"); rd_kafka_topic_partition_list_destroy(list); }
bool KafkaConsumer::assigned() const noexcept { return true; }
}
