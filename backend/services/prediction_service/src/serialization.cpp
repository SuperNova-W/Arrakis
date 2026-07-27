#include "arrakis/prediction/prediction_service.hpp"
#include "feature_event.pb.h"
#include "model_prediction.pb.h"

#include <chrono>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <limits>

namespace arrakis::prediction {
namespace {
template <typename Message> std::vector<std::byte> encode(const Message& message) {
    std::string bytes; if (!message.SerializeToString(&bytes)) throw std::runtime_error("protobuf serialization failed");
    std::vector<std::byte> result(bytes.size()); std::memcpy(result.data(), bytes.data(), bytes.size()); return result;
}
std::int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
}
FeatureInput deserialize_feature(std::span<const std::byte> bytes) {
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) throw std::invalid_argument("feature payload size is invalid");
    market::features::v1::FeatureEvent message;
    if (!message.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) throw std::invalid_argument("invalid FeatureEvent protobuf");
    if (message.event_id().empty() || message.target_symbol().empty() || message.feature_version().empty() || message.feature_schema_hash().empty() || message.features().empty() || !message.complete_context()) throw std::invalid_argument("FeatureEvent identity, schema, or completeness is invalid");
    FeatureInput result{message.event_id(), message.target_symbol(), message.event_time_unix_ms(), message.feature_version(), message.feature_schema_hash(), {}, {}};
    const auto feature_count = static_cast<std::size_t>(message.features_size());
    result.names.reserve(feature_count); result.values.reserve(feature_count);
    for (const auto& item : message.features()) {
        if (item.name().empty() || !std::isfinite(item.value())) throw std::invalid_argument("FeatureEvent contains an invalid feature");
        result.names.push_back(item.name()); result.values.push_back(static_cast<float>(item.value()));
    }
    return result;
}
std::vector<std::byte> serialize_prediction(const model::Prediction& prediction, const FeatureInput& input) {
    ::model::predictions::v1::PredictionEvent message;
    message.set_event_id(prediction.model_id + ":" + input.event_id);
    message.set_feature_event_id(input.event_id); message.set_target_symbol(input.target_symbol); message.set_event_time_unix_ms(input.event_time_unix_ms); message.set_produced_time_unix_ms(now_ms());
    message.set_model_id(prediction.model_id); message.set_model_version("v001"); message.set_feature_version(input.feature_version); message.set_feature_schema_hash(input.schema_hash); message.set_predicted_return(prediction.predicted_return); message.set_prediction_horizon_bars(prediction.prediction_horizon_bars);
    return encode(message);
}
}
