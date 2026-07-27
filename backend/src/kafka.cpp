#include "arrakis/streaming/kafka.hpp"

#include <librdkafka/rdkafka.h>

#include <stdexcept>
#include <string>
#include <cstring>
#include <cstdio>
#include <atomic>

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
    else { set("enable.auto.commit", "false"); set("auto.offset.reset", "earliest"); }
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
