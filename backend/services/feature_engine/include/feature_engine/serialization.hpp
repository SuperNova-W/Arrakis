#pragma once

#include "feature_engine/feature_engine.hpp"
#include "arrakis/streaming/kafka.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace arrakis::feature_engine {
[[nodiscard]] std::vector<std::byte> serialize_feature(const FeatureVector& feature);
[[nodiscard]] std::vector<std::byte> serialize_error(const streaming::KafkaRecord& record, std::string_view service, std::string_view code, std::string_view description, std::span<const std::byte> original, std::span<const std::string> missing = {});
}
