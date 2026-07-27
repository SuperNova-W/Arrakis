#pragma once

#include "arrakis/model/sector_ml.hpp"
#include "arrakis/prediction/config.hpp"
#include "arrakis/streaming/kafka.hpp"
#include "arrakis/runtime/metrics.hpp"

#include <span>
#include <string>
#include <vector>

namespace arrakis::prediction {
struct FeatureInput final {
    std::string event_id;
    std::string target_symbol;
    std::int64_t event_time_unix_ms{};
    std::string feature_version;
    std::string schema_hash;
    std::vector<std::string> names;
    std::vector<float> values;
};

[[nodiscard]] FeatureInput deserialize_feature(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> serialize_prediction(
    const model::Prediction& prediction, const FeatureInput& input);

class PredictionService final {
public:
    PredictionService(ServiceConfig config, runtime::Metrics& metrics);
    [[nodiscard]] std::vector<std::byte> predict(std::span<const std::byte> feature_payload) const;
private:
    ServiceConfig config_;
    runtime::Metrics& metrics_;
    model::SectorXGBoostModel model_;
};
}
