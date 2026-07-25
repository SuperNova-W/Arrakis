#include "arrakis/model/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

namespace arrakis::model {
namespace {

[[nodiscard]] double roc_auc(
    const std::vector<float>& labels,
    const std::vector<float>& probabilities
) {
    std::vector<std::pair<float, float>> ranked;
    ranked.reserve(labels.size());
    for (std::size_t index = 0; index < labels.size(); ++index) {
        ranked.emplace_back(probabilities[index], labels[index]);
    }
    std::ranges::sort(ranked, {}, &std::pair<float, float>::first);

    double positive_rank_sum = 0.0;
    std::size_t positive_count = 0;
    std::size_t index = 0;
    while (index < ranked.size()) {
        auto tie_end = index + 1;
        while (tie_end < ranked.size() && ranked[tie_end].first == ranked[index].first) {
            ++tie_end;
        }

        // Ranks are one-based. Every item in a tie receives the average rank.
        const auto average_rank =
            (static_cast<double>(index + 1) + static_cast<double>(tie_end)) / 2.0;
        for (auto tie_index = index; tie_index < tie_end; ++tie_index) {
            if (ranked[tie_index].second == 1.0F) {
                positive_rank_sum += average_rank;
                ++positive_count;
            }
        }
        index = tie_end;
    }

    const auto negative_count = labels.size() - positive_count;
    if (positive_count == 0 || negative_count == 0) {
        throw std::invalid_argument{"ROC AUC requires both positive and negative labels"};
    }

    const auto positives = static_cast<double>(positive_count);
    const auto negatives = static_cast<double>(negative_count);
    return (positive_rank_sum - positives * (positives + 1.0) / 2.0) /
           (positives * negatives);
}

}  // namespace

BinaryMetrics evaluate_binary_classifier(
    const std::vector<float>& labels,
    const std::vector<float>& probabilities,
    const double threshold
) {
    if (labels.empty() || labels.size() != probabilities.size()) {
        throw std::invalid_argument{"Labels and probabilities must be non-empty and equally sized"};
    }
    if (!(threshold > 0.0 && threshold < 1.0)) {
        throw std::invalid_argument{"Threshold must be between 0 and 1"};
    }

    constexpr double epsilon = 1.0e-7;
    std::size_t correct = 0;
    double loss_sum = 0.0;
    double label_sum = 0.0;
    double probability_sum = 0.0;

    for (std::size_t index = 0; index < labels.size(); ++index) {
        const auto label = static_cast<double>(labels[index]);
        const auto probability = static_cast<double>(probabilities[index]);
        if ((label != 0.0 && label != 1.0) || !std::isfinite(probability) ||
            probability < 0.0 || probability > 1.0) {
            throw std::invalid_argument{"Invalid binary label or probability"};
        }

        const auto bounded = std::clamp(probability, epsilon, 1.0 - epsilon);
        loss_sum -= label * std::log(bounded) + (1.0 - label) * std::log(1.0 - bounded);
        label_sum += label;
        probability_sum += probability;
        correct += static_cast<std::size_t>((probability >= threshold) == (label == 1.0));
    }

    const auto count = static_cast<double>(labels.size());
    return BinaryMetrics{
        .accuracy = static_cast<double>(correct) / count,
        .log_loss = loss_sum / count,
        .roc_auc = roc_auc(labels, probabilities),
        .positive_rate = label_sum / count,
        .mean_probability = probability_sum / count,
    };
}

}  // namespace arrakis::model
