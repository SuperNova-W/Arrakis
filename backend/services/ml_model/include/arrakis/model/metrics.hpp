#pragma once

#include <vector>

namespace arrakis::model {

struct BinaryMetrics final {
    double accuracy;
    double log_loss;
    double roc_auc;
    double positive_rate;
    double mean_probability;
};

[[nodiscard]] BinaryMetrics evaluate_binary_classifier(
    const std::vector<float>& labels,
    const std::vector<float>& probabilities,
    double threshold = 0.5
);

}  // namespace arrakis::model
