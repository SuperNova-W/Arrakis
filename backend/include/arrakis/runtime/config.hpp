#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace arrakis::runtime {

struct IngestionConfig {
    std::string websocket_host{"ws.finnhub.io"};
    std::string websocket_port{"443"};
    std::string websocket_path{"/"};
    std::string raw_trade_topic{"market.raw.trades"};
    std::string producer_name{"finnhub-ingestion-v1"};
    std::string schema_version{"trade-event-v1"};
    std::string universe_config{"config/etf_universe.json"};
    std::uint32_t initial_reconnect_delay_ms{1000};
    std::uint32_t maximum_reconnect_delay_ms{60000};
    std::uint32_t reconnect_jitter_percent{20};
    std::uint16_t metrics_port{9101};
    std::vector<std::string> symbols;
};

struct BarConfig {
    std::string input_topic{"market.raw.trades"};
    std::string consumer_group{"bar-aggregation-v1"};
    std::int64_t bar_interval_seconds{60};
    std::int64_t allowed_lateness_seconds{5};
    std::size_t deduplication_window_minutes{10};
    std::uint16_t metrics_port{9102};
};

[[nodiscard]] IngestionConfig load_ingestion_config(const std::string& path);
[[nodiscard]] BarConfig load_bar_config(const std::string& path);
[[nodiscard]] std::vector<std::string> default_etf_universe();

}  // namespace arrakis::runtime
