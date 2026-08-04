#include "arrakis/model/dataset.hpp"
#include "arrakis/model/metrics.hpp"
#include "arrakis/model/sector_ml.hpp"

#include <cmath>
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef ARRAKIS_SAMPLE_DATASET_PATH
#define ARRAKIS_SAMPLE_DATASET_PATH "services/ml_model/data/sample_features.csv"
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

    const auto exact = arrakis::model::chronological_split_by_dates(
        dataset, "2026-01-06", "2026-01-08", "2026-01-10"
    );
    require(exact.train.row_count() == 6, "Exact split training boundary moved");
    require(exact.validation.row_count() == 2, "Exact split validation boundary moved");
    require(exact.test.row_count() == 2, "Exact split test boundary moved");
    require(exact.test.dates.front() == "2026-01-09", "Exact split test start moved");
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

void test_sector_feature_builder_and_split() {
    std::vector<arrakis::model::BarRecord> bars;
    bars.reserve(24);

    for (int index = 0; index < 24; ++index) {
        const auto timestamp = "2024-01-" + std::to_string(1 + index / 2);
        const auto close = 100.0 + static_cast<double>(index) * 0.2;
        const auto volume = 1000.0 + static_cast<double>(index) * 5.0;
        bars.push_back({
            .timestamp = timestamp,
            .symbol = "XLK",
            .open = close - 0.1,
            .high = close + 0.1,
            .low = close - 0.2,
            .close = close,
            .volume = volume,
        });
    }

    for (int index = 0; index < 24; ++index) {
        const auto timestamp = "2024-01-" + std::to_string(1 + index / 2);
        const auto close = 100.0 + static_cast<double>(index) * 0.15;
        const auto volume = 2000.0 + static_cast<double>(index) * 2.0;
        bars.push_back({
            .timestamp = timestamp,
            .symbol = "SPY",
            .open = close - 0.1,
            .high = close + 0.1,
            .low = close - 0.2,
            .close = close,
            .volume = volume,
        });
    }

    arrakis::model::FeatureConfiguration config;
    config.target_symbol = "XLK";
    config.prediction_horizon_bars = 2;
    config.lookback_window = 6;
    config.training_start = "2024-01-01";
    config.validation_start = "2024-01-16";

    arrakis::model::SectorFeatureBuilder builder{config};
    const auto dataset = builder.build(bars);

    require(dataset.row_count() > 0, "Expected training rows from feature builder");
    require(dataset.feature_names.size() >= 8, "Expected engineered sector features");
    require(dataset.labels.size() == dataset.row_count(), "Labels must align with rows");
    require(dataset.dates.front() != dataset.dates.back(), "Expected more than one timestamp");

    const auto split = builder.build_split(dataset, 0.2, 1, 0);
    require(split.train.row_count() > 0, "Expected train split rows");
    require(split.validation.row_count() > 0, "Expected validation split rows");
}

void test_sector_training_and_inference_roundtrip() {
    std::vector<arrakis::model::BarRecord> bars;
    bars.reserve(48);

    for (int index = 0; index < 24; ++index) {
        const auto timestamp = "2024-01-" + std::to_string(1 + index / 2);
        const auto close = 100.0 + static_cast<double>(index) * 0.1;
        const auto volume = 1000.0 + static_cast<double>(index) * 10.0;
        bars.push_back({
            .timestamp = timestamp,
            .symbol = "XLK",
            .open = close - 0.05,
            .high = close + 0.05,
            .low = close - 0.1,
            .close = close,
            .volume = volume,
        });
    }

    for (int index = 0; index < 24; ++index) {
        const auto timestamp = "2024-01-" + std::to_string(1 + index / 2);
        const auto close = 100.0 + static_cast<double>(index) * 0.08;
        const auto volume = 1200.0 + static_cast<double>(index) * 8.0;
        bars.push_back({
            .timestamp = timestamp,
            .symbol = "SPY",
            .open = close - 0.05,
            .high = close + 0.05,
            .low = close - 0.1,
            .close = close,
            .volume = volume,
        });
    }

    arrakis::model::FeatureConfiguration config;
    config.target_symbol = "XLK";
    config.prediction_horizon_bars = 2;
    config.lookback_window = 6;
    config.training_start = "2024-01-01";
    config.validation_start = "2024-01-16";
    config.max_rounds = 5;
    config.early_stopping_rounds = 2;

    arrakis::model::SectorFeatureBuilder builder{config};
    const auto dataset = builder.build(bars);
    const auto split = builder.build_split(dataset, 0.2, 1, 0);

    const auto output_dir = std::filesystem::temp_directory_path() / "arrakis_sector_model_test";
    std::filesystem::remove_all(output_dir);
    std::filesystem::create_directories(output_dir);

    arrakis::model::SectorXGBoostTrainer trainer{config};
    const auto model_path = trainer.train(split.train, output_dir);
    require(std::filesystem::exists(model_path), "Expected model artifact to be written");

    arrakis::model::SectorXGBoostModel model;
    model.load(
        output_dir / "model.ubj",
        output_dir / "metadata.json",
        output_dir / "feature_schema.json"
    );

    std::vector<float> row(split.validation.features.begin(), split.validation.features.begin() + split.validation.feature_count());
    const auto prediction = model.predict(row);
    require(prediction.model_id == "xgb_xlk", "Unexpected model identifier");
    require(std::isfinite(prediction.predicted_return), "Prediction should be finite");
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
