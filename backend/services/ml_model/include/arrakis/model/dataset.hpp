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

}  // namespace arrakis::model
