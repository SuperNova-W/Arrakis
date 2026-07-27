#include "arrakis/prediction/prediction_service.hpp"

#include <algorithm>
#include <stdexcept>

namespace arrakis::prediction {
PredictionService::PredictionService(ServiceConfig config, runtime::Metrics& metrics) : config_(std::move(config)), metrics_(metrics) {
    model_.load(config_.model_path, config_.metadata_path, config_.schema_path);
    if (model_.target_symbol() != config_.target_symbol) throw std::runtime_error("loaded model target symbol does not match service configuration");
}
std::vector<std::byte> PredictionService::predict(std::span<const std::byte> payload) const {
    const auto input = deserialize_feature(payload);
    if (input.target_symbol != config_.target_symbol || input.feature_version != model_.feature_version() || (!config_.feature_schema_hash.empty() && input.schema_hash != config_.feature_schema_hash) || (!model_.feature_schema_hash().empty() && input.schema_hash != model_.feature_schema_hash())) {
        metrics_.increment("prediction_events_schema_rejected_total"); throw std::invalid_argument("FeatureEvent schema does not match loaded XLK model");
    }
    if (input.names != model_.feature_names()) { metrics_.increment("prediction_events_schema_rejected_total"); throw std::invalid_argument("FeatureEvent feature ordering does not match model schema"); }
    auto prediction = model_.predict(input.values); prediction.symbol = input.target_symbol; prediction.event_time = std::to_string(input.event_time_unix_ms); metrics_.increment("prediction_events_processed_total");
    return serialize_prediction(prediction, input);
}
}
