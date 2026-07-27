#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace arrakis::runtime {
class Metrics {
public:
    Metrics();
    ~Metrics();
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;
    void increment(const std::string& name, std::uint64_t value = 1);
    void set(const std::string& name, std::int64_t value);
    [[nodiscard]] std::string render() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class MetricsServer {
public:
    MetricsServer(Metrics& metrics, std::uint16_t port);
    ~MetricsServer();
    MetricsServer(const MetricsServer&) = delete;
    MetricsServer& operator=(const MetricsServer&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace arrakis::runtime
