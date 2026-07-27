#include "feature_engine/config.hpp"

#include <boost/json.hpp>
#include <fstream>
#include <stdexcept>

namespace arrakis::feature_engine {
namespace {
boost::json::object read(const std::string& path) {
    std::ifstream input(path); if (!input) throw std::runtime_error("cannot open feature configuration: " + path);
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()); boost::system::error_code error; auto value=boost::json::parse(text,error);
    if(error||!value.is_object()) throw std::runtime_error("invalid feature configuration: " + path); return value.as_object();
}
void string_field(const boost::json::object& object,const char* key,std::string& destination){if(const auto* value=object.if_contains(key)){if(!value->is_string())throw std::runtime_error(std::string("feature configuration field is not a string: ")+key);destination=value->as_string();}}
std::vector<std::string> string_array(const boost::json::object& object,const char* key){std::vector<std::string> result;const auto* value=object.if_contains(key);if(value==nullptr)return result;if(!value->is_array())throw std::runtime_error(std::string("feature configuration field is not an array: ")+key);for(const auto& item:value->as_array()){if(!item.is_string())throw std::runtime_error(std::string("feature symbol is not a string: ")+key);result.emplace_back(item.as_string());}return result;}
}
ServiceConfig load_config(const std::string& path){const auto object=read(path);ServiceConfig result;string_field(object,"input_topic",result.input_topic);string_field(object,"output_topic",result.output_topic);string_field(object,"error_topic",result.error_topic);string_field(object,"dead_letter_topic",result.dead_letter_topic);string_field(object,"consumer_group",result.consumer_group);string_field(object,"feature_version",result.features.feature_version);string_field(object,"feature_schema_hash",result.features.feature_schema_hash);if(const auto* value=object.if_contains("context_wait_timeout_seconds"))result.context_wait_timeout=std::chrono::seconds(value->to_number<int>());if(const auto* value=object.if_contains("maximum_history_bars"))result.features.maximum_history_bars=value->to_number<std::size_t>();if(const auto* value=object.if_contains("deduplication_window_minutes"))result.features.deduplication_window_minutes=value->to_number<std::size_t>();if(const auto* value=object.if_contains("metrics_port"))result.metrics_port=value->to_number<std::uint16_t>();result.features.sector_symbols=string_array(object,"sector_symbols");result.features.context_symbols=string_array(object,"context_symbols");if(result.features.sector_symbols.empty()||result.features.context_symbols.empty())throw std::runtime_error("feature ETF universe cannot be empty");return result;}
}
