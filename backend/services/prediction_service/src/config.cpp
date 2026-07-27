#include "arrakis/prediction/config.hpp"

#include <boost/json.hpp>
#include <fstream>
#include <stdexcept>

namespace arrakis::prediction {
namespace {
boost::json::object read(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open prediction config: " + path.string());
    std::string text((std::istreambuf_iterator<char>(input)), {});
    return boost::json::parse(text).as_object();
}
void string_value(const boost::json::object& object, const char* key, std::string& target) {
    if (const auto* value = object.if_contains(key)) target = value->as_string().c_str();
}
}
ServiceConfig load_config(const std::filesystem::path& path) {
    const auto object = read(path); ServiceConfig result;
    string_value(object, "input_topic", result.input_topic); string_value(object, "output_topic", result.output_topic);
    string_value(object, "dead_letter_topic", result.dead_letter_topic); string_value(object, "consumer_group", result.consumer_group);
    string_value(object, "target_symbol", result.target_symbol); string_value(object, "feature_version", result.feature_version);
    string_value(object, "feature_schema_hash", result.feature_schema_hash);
    if (const auto* value = object.if_contains("model_path")) result.model_path = value->as_string().c_str();
    if (const auto* value = object.if_contains("metadata_path")) result.metadata_path = value->as_string().c_str();
    if (const auto* value = object.if_contains("schema_path")) result.schema_path = value->as_string().c_str();
    if (const auto* value = object.if_contains("metrics_port")) result.metrics_port = value->to_number<std::uint16_t>();
    if (result.model_path.empty() || result.metadata_path.empty() || result.schema_path.empty()) throw std::runtime_error("prediction model artifact paths are required");
    return result;
}
}
