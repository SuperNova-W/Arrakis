#include "arrakis/model/dataset.hpp"
#include "arrakis/model/metrics.hpp"
#include "arrakis/model/sector_ml.hpp"
#include "arrakis/news/xlk_membership.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef ARRAKIS_SAMPLE_DATASET_PATH
#define ARRAKIS_SAMPLE_DATASET_PATH "services/ml_model/data/sample_features.csv"
#endif

#ifndef ARRAKIS_XLK_HOLDINGS_HISTORY_PATH
#define ARRAKIS_XLK_HOLDINGS_HISTORY_PATH "data/metadata/xlk_holdings_history.csv"
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

    const auto window = arrakis::model::date_slice(dataset, "2026-01-03", "2026-01-05");
    require(window.row_count() == 3, "Date slice row count moved");
    require(window.dates.front() == "2026-01-03" && window.dates.back() == "2026-01-05",
            "Date slice boundaries moved");

    const auto rows = arrakis::model::row_slice(dataset, 2, 5);
    require(rows.row_count() == 3, "Row slice row count moved");
    require(rows.dates.front() == "2026-01-03" && rows.dates.back() == "2026-01-05",
            "Row slice boundaries moved");
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

// Regression coverage for the FNSPID importer's point-in-time XLK membership.
//
// `arrakis-import-fnspid` used to build per-symbol intervals that were closed at
// the next row for the same symbol, leaving the final row of every symbol open
// on the file's 2099-12-31 sentinel. That kept departed constituents "held"
// forever and bridged re-entry gaps. The importer now links the single shared
// resolver exercised below, so these assertions cover the importer's filter.
void test_point_in_time_xlk_membership() {
    const auto history =
        arrakis::news::XlkMembershipResolver::from_csv(ARRAKIS_XLK_HOLDINGS_HISTORY_PATH);

    require(history.snapshots().size() == 17, "Expected 17 quarterly N-PORT snapshots");
    require(history.first_snapshot_date() == "2019-09-30", "Unexpected first snapshot date");
    require(history.last_snapshot_date() == "2023-09-30", "Unexpected last snapshot date");

    // 1. Survivorship / lookahead. V and MA last appear in the 2022-12-31
    // filing; they left XLK in the March 2023 GICS reclassification. The whole
    // of 2023 is the test year in every published evaluation, so treating them
    // as constituents there contaminated the test set.
    for (const auto* symbol :
         {"V", "MA", "PYPL", "ADP", "PAYX", "FIS", "FISV", "GPN", "BR", "JKHY"}) {
        require(
            history.held_on(symbol, "2023-03-30"),
            std::string{symbol} + " should still be held on 2023-03-30"
        );
        require(
            !history.held_on(symbol, "2023-03-31"),
            std::string{symbol} + " must not be held on 2023-03-31, the first day of the "
                                  "snapshot that dropped it"
        );
        require(
            !history.held_on(symbol, "2023-12-28"),
            std::string{symbol} + " must not be held anywhere in the 2023 test year after the "
                                  "reclassification"
        );
    }

    // Departures at other snapshot boundaries must behave the same way. The old
    // importer carried every one of these forward to 2099-12-31.
    struct Departure final {
        const char* symbol;
        const char* last_covered_date;  // last date the symbol is still a constituent
        const char* first_absent_date;  // first day of the snapshot that dropped it
    };
    for (const auto& departure : {
             Departure{"LDOS", "2021-03-30", "2021-03-31"},
             Departure{"VNT", "2021-03-30", "2021-03-31"},
             Departure{"XRXDW", "2021-03-30", "2021-03-31"},
             Departure{"IPGP", "2022-06-29", "2022-06-30"},
             Departure{"PAYC", "2023-06-29", "2023-06-30"},
         }) {
        require(
            history.held_on(departure.symbol, departure.last_covered_date),
            std::string{departure.symbol} + " should be held on " + departure.last_covered_date
        );
        require(
            !history.held_on(departure.symbol, departure.first_absent_date),
            std::string{departure.symbol} + " must not be held on " + departure.first_absent_date
        );
        require(
            !history.held_on(departure.symbol, "2023-12-28"),
            std::string{departure.symbol} + " must not be carried forward to the end of the test year"
        );
    }

    // 2. Re-entry gaps must not be bridged. CSCO is absent from all four 2021
    // filings and returns in the 2022-03-31 filing.
    require(history.held_on("CSCO", "2021-03-30"), "CSCO should be held up to 2021-03-30");
    for (const auto* date : {"2021-03-31", "2021-06-30", "2021-09-30", "2021-12-31",
                             "2022-03-30"}) {
        require(
            !history.held_on("CSCO", date),
            std::string{"CSCO must not be held on "} + date + " (absent from the 2021 filings)"
        );
    }
    require(history.held_on("CSCO", "2022-03-31"), "CSCO should be held again from 2022-03-31");

    // 3. The shipped CSV was assembled by concatenation and repeats its own
    // header. The old importer ingested that row as a holding for a symbol
    // literally named "symbol".
    for (const auto& snapshot : history.snapshots()) {
        for (const auto& constituent : snapshot.constituents) {
            require(
                constituent.symbol != "symbol" && constituent.symbol != "effective_from",
                "The repeated header row must never be ingested as a constituent"
            );
        }
        require(snapshot.constituents.size() >= 30, "Implausibly small XLK snapshot");
    }
    require(
        !history.held_on("symbol", "2023-06-30"),
        "A symbol named 'symbol' must never resolve as held"
    );
    // The duplicated first AAPL row is collapsed rather than double-counted.
    const auto& first = history.constituents_on("2019-09-30");
    require(
        std::count_if(
            first.begin(), first.end(),
            [](const auto& constituent) { return constituent.symbol == "AAPL"; }
        ) == 1,
        "The duplicated AAPL row must be collapsed to a single constituent"
    );

    // 4. No current-holdings fallback before the first filing.
    require(
        history.constituents_on("2019-09-29").empty(),
        "Membership before the first snapshot must be empty"
    );
    require(
        history.constituents_on("2019-01-02").empty(),
        "Membership before the first snapshot must be empty"
    );
    require(!history.held_on("AAPL", "2019-09-29"), "AAPL must not be held before 2019-09-30");
    require(
        !history.governing_snapshot("2019-09-29").has_value(),
        "No snapshot governs a date before the first filing"
    );

    // 5. Forward carry of the final filing is flagged rather than hidden.
    require(
        !history.is_extrapolated_forward("2023-09-29"),
        "2023-09-29 is covered by a filing, not extrapolated"
    );
    require(
        history.is_extrapolated_forward("2023-12-28"),
        "Dates after the last filing must be flagged as carried forward"
    );
}

}  // namespace

int main() {
    try {
        test_chronological_split();
        test_binary_metrics();
        test_sample_dataset_contract();
        test_point_in_time_xlk_membership();
        std::cout << "All model-core tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
