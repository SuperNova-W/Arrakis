#include "feature_engine/serialization.hpp"
#include "feature_event.pb.h"
#include "feature_error_event.pb.h"

#include <chrono>
#include <cstring>
#include <stdexcept>

namespace arrakis::feature_engine {
namespace {
template <typename Message> std::vector<std::byte> encode(const Message& message) { std::string bytes; if(!message.SerializeToString(&bytes))throw std::runtime_error("feature protobuf serialization failed"); std::vector<std::byte> result(bytes.size()); std::memcpy(result.data(),bytes.data(),bytes.size());return result; }
std::int64_t now_ms(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
std::string event_id(const FeatureVector& feature){return feature.target_symbol+":"+feature.feature_version+":"+std::to_string(feature.event_time.time_since_epoch().count());}
}
std::vector<std::byte> serialize_feature(const FeatureVector& feature){::market::features::v1::FeatureEvent message;message.set_event_id(event_id(feature));message.set_source_bar_event_id(feature.source_bar_event_id);message.set_target_symbol(feature.target_symbol);message.set_event_time_unix_ms(feature.event_time.time_since_epoch().count());message.set_produced_time_unix_ms(now_ms());message.set_feature_version(feature.feature_version);message.set_feature_schema_hash(feature.schema_hash);for(std::size_t i=0;i<feature.names.size();++i){auto* item=message.add_features();item->set_name(feature.names[i]);item->set_value(feature.values[i]);}for(const auto& symbol:feature.source_symbols)message.add_source_symbols(symbol);message.set_complete_context(feature.complete_context);return encode(message);}
std::vector<std::byte> serialize_error(const streaming::KafkaRecord& record,std::string_view service,std::string_view code,std::string_view description,std::span<const std::byte> original,std::span<const std::string> missing){::market::features::v1::FeatureErrorEvent message;message.set_event_id(std::string(service)+":error:"+std::to_string(record.offset));message.set_source_topic(record.topic);message.set_partition(record.partition);message.set_offset(record.offset);message.set_service(service);message.set_error_code(code);message.set_error_description(description);message.set_original_payload(original.data(),original.size());for(const auto& symbol:missing)message.add_missing_symbols(symbol);return encode(message);}
}
