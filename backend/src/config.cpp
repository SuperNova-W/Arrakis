#include "arrakis/runtime/config.hpp"

#include <boost/json.hpp>
#include <fstream>
#include <stdexcept>

namespace arrakis::runtime {
namespace {
boost::json::object read_object(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open configuration: " + path);
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    boost::system::error_code error;
    auto value = boost::json::parse(text, error);
    if (error || !value.is_object()) throw std::runtime_error("invalid JSON configuration: " + path);
    return value.as_object();
}
template <typename T> void optional(const boost::json::object& object, const char* key, T& target) {
    if (const auto* value = object.if_contains(key)) target = value->to_number<T>();
}
void optional_string(const boost::json::object& object, const char* key, std::string& target) {
    if (const auto* value = object.if_contains(key)) { if (!value->is_string()) throw std::runtime_error(std::string("configuration field is not a string: ") + key); target = value->as_string(); }
}
void symbols(const boost::json::object& object, std::vector<std::string>& target) {
    if (const auto* value = object.if_contains("symbols")) {
        if (!value->is_array()) throw std::runtime_error("configuration symbols must be an array");
        target.clear(); for (const auto& item : value->as_array()) { if (!item.is_string()) throw std::runtime_error("configuration symbol must be a string"); target.emplace_back(item.as_string()); }
    }
}
}
std::vector<std::string> default_etf_universe() { return {"XLC","XLY","XLP","XLE","XLF","XLV","XLI","XLB","XLRE","XLK","XLU","SPY","QQQ","IWM","TLT","HYG","GLD","USO"}; }
IngestionConfig load_ingestion_config(const std::string& path) { auto object = read_object(path); IngestionConfig config; optional_string(object,"websocket_host",config.websocket_host); optional_string(object,"websocket_port",config.websocket_port); optional_string(object,"websocket_path",config.websocket_path); optional_string(object,"raw_trade_topic",config.raw_trade_topic); optional_string(object,"dead_letter_topic",config.dead_letter_topic); optional_string(object,"producer_name",config.producer_name); optional_string(object,"schema_version",config.schema_version); optional(object,"initial_reconnect_delay_ms",config.initial_reconnect_delay_ms); optional(object,"maximum_reconnect_delay_ms",config.maximum_reconnect_delay_ms); optional(object,"reconnect_jitter_percent",config.reconnect_jitter_percent); optional(object,"metrics_port",config.metrics_port); symbols(object,config.symbols); if (config.symbols.empty()) config.symbols = default_etf_universe(); if (config.initial_reconnect_delay_ms == 0 || config.maximum_reconnect_delay_ms < config.initial_reconnect_delay_ms || config.reconnect_jitter_percent > 100) throw std::runtime_error("invalid ingestion reconnect configuration"); return config; }
BarConfig load_bar_config(const std::string& path) { auto object = read_object(path); BarConfig config; optional_string(object,"input_topic",config.input_topic); optional_string(object,"output_topic",config.output_topic); optional_string(object,"late_trade_topic",config.late_trade_topic); optional_string(object,"dead_letter_topic",config.dead_letter_topic); optional_string(object,"consumer_group",config.consumer_group); optional_string(object,"schema_version",config.schema_version); optional(object,"bar_interval_seconds",config.bar_interval_seconds); optional(object,"allowed_lateness_seconds",config.allowed_lateness_seconds); optional(object,"deduplication_window_minutes",config.deduplication_window_minutes); optional(object,"metrics_port",config.metrics_port); if (config.bar_interval_seconds <= 0 || config.allowed_lateness_seconds < 0 || config.deduplication_window_minutes == 0) throw std::runtime_error("invalid bar configuration"); return config; }
}
