#include "arrakis/model/dataset.hpp"
#include "arrakis/model/metrics.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef ARRAKIS_SAMPLE_DATASET_PATH
#define ARRAKIS_SAMPLE_DATASET_PATH "data/sample_features.csv"
#endif

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

void test_chronological_split() {
    arrakis::model::Dataset dataset{
        .dates = {"2026-01-01", "2026-01-02", "2026-01-03", "2026-01-04", "2026-01-05",
                  "2026-01-06", "2026-01-07", "2026-01-08", "2026-01-09", "2026-01-10"},
        .feature_names = {"momentum"},
        .features = {0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F, 0.7F, 0.8F, 0.9F, 1.0F},
        .labels = {0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 1.0F},
    };

    const auto split = arrakis::model::chronological_split(dataset, 0.2);
    require(split.train.row_count() == 8, "Expected 8 training rows");
    require(split.validation.row_count() == 2, "Expected 2 validation rows");
    require(split.train.dates.back() == "2026-01-08", "Training boundary moved");
    require(split.validation.dates.front() == "2026-01-09", "Validation boundary moved");
}

void test_binary_metrics() {
    const auto metrics = arrakis::model::evaluate_binary_classifier(
        {0.0F, 0.0F, 1.0F, 1.0F},
        {0.1F, 0.4F, 0.6F, 0.9F}
    );

    require(std::abs(metrics.accuracy - 1.0) < 1.0e-9, "Accuracy should be perfect");
    require(std::abs(metrics.roc_auc - 1.0) < 1.0e-9, "ROC AUC should be perfect");
    require(std::abs(metrics.positive_rate - 0.5) < 1.0e-9, "Positive rate should be 0.5");
}

void test_sample_dataset_contract() {
    const auto dataset = arrakis::model::load_csv(
        ARRAKIS_SAMPLE_DATASET_PATH,
        "target_up_5d"
    );

    require(dataset.row_count() == 40, "Expected 40 sample rows");
    require(dataset.feature_count() == 4, "Expected 4 sample features");
    require(dataset.dates.front() == "2026-01-02", "Unexpected first sample date");
    require(dataset.dates.back() == "2026-03-02", "Unexpected last sample date");
}

}  // namespace

int main() {
    try {
        test_chronological_split();
        test_binary_metrics();
        test_sample_dataset_contract();
        std::cout << "All model-core tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
