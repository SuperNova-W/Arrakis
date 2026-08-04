#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace arrakis::model {

// Dense, row-major feature data. Dates remain as ISO-8601 strings because this
// first training slice only needs to validate ordering and report boundaries.
struct Dataset final {
    std::vector<std::string> dates;
    std::vector<std::string> feature_names;
    std::vector<float> features;
    std::vector<float> labels;

    [[nodiscard]] std::size_t row_count() const noexcept;
    [[nodiscard]] std::size_t feature_count() const noexcept;
};

struct DatasetSplit final {
    Dataset train;
    Dataset validation;
};

struct DatasetThreeWaySplit final {
    Dataset train;
    Dataset validation;
    Dataset test;
};

// Expected CSV shape:
//   date,<numeric feature columns>,<target column>
//
// The date column must be named "date" and contain lexicographically sortable
// ISO-8601 dates. Missing feature values may be empty or "nan". Labels must be
// exactly 0 or 1. Quoted fields are intentionally not supported yet.
[[nodiscard]] Dataset load_csv(
    const std::filesystem::path& path,
    const std::string& target_column
);

// Preserves temporal ordering: the earlier rows train the model and the most
// recent rows form validation. No rows are shuffled.
[[nodiscard]] DatasetSplit chronological_split(
    const Dataset& dataset,
    double validation_fraction
);

// Uses inclusive ISO date boundaries and preserves the original ordering.
// Rows outside the requested windows are rejected rather than silently placed
// into a neighboring split.
[[nodiscard]] DatasetThreeWaySplit chronological_split_by_dates(
    const Dataset& dataset,
    const std::string& train_end,
    const std::string& validation_end,
    const std::string& test_end
);

}  // namespace arrakis::model
