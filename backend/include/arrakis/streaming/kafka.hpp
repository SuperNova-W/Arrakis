#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace arrakis::streaming {

struct KafkaRecord {
    std::string topic;
    std::string key;
    std::vector<std::byte> payload;
    std::int32_t partition{};
    std::int64_t offset{};
};

class KafkaProducer {
public:
    explicit KafkaProducer(std::string_view brokers, std::string_view client_id);
    ~KafkaProducer();
    KafkaProducer(const KafkaProducer&) = delete;
    KafkaProducer& operator=(const KafkaProducer&) = delete;

    void publish(std::string_view topic, std::string_view key, std::span<const std::byte> payload);
    void poll_events(std::chrono::milliseconds timeout);
    void flush(std::chrono::milliseconds timeout);
    [[nodiscard]] bool usable() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class KafkaConsumer {
public:
    KafkaConsumer(std::string_view brokers, std::string_view group, std::string_view topic);
    ~KafkaConsumer();
    KafkaConsumer(const KafkaConsumer&) = delete;
    KafkaConsumer& operator=(const KafkaConsumer&) = delete;

    [[nodiscard]] std::optional<KafkaRecord> poll(std::chrono::milliseconds timeout);
    void commit(const KafkaRecord& record);
    [[nodiscard]] bool assigned() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace arrakis::streaming
