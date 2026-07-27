#pragma once

#include "feature_engine/feature_engine.hpp"

#include <chrono>
#include <cstddef>
#include <string>

namespace arrakis::feature_engine {

struct ServiceConfig {
    std::string input_topic{"market.bars.5m"};
    std::string output_topic{"market.features"};
    std::string error_topic{"market.feature.errors"};
    std::string dead_letter_topic{"dead-letter.events"};
    std::string consumer_group{"feature-engine-v1"};
    std::chrono::seconds context_wait_timeout{10};
    std::uint16_t metrics_port{9103};
    FeatureConfig features;
};

[[nodiscard]] ServiceConfig load_config(const std::string& path);

}  // namespace arrakis::feature_engine
