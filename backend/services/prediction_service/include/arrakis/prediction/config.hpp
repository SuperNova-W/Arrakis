#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace arrakis::prediction {
struct ServiceConfig final {
    std::string input_topic{"market.features"};
    std::string output_topic{"model.predictions"};
    std::string dead_letter_topic{"dead-letter.events"};
    std::string consumer_group{"xgb-prediction-v1"};
    std::string target_symbol{"XLK"};
    std::string feature_version{"sector-features-v1"};
    std::string feature_schema_hash{};
    std::filesystem::path model_path;
    std::filesystem::path metadata_path;
    std::filesystem::path schema_path;
    std::uint16_t metrics_port{9104};
};

[[nodiscard]] ServiceConfig load_config(const std::filesystem::path& path);
}
