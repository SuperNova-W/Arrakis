#include "arrakis/model/dataset.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace arrakis::model {
namespace {

[[nodiscard]] std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (character == '"') {
            if (quoted && index + 1 < line.size() && line[index + 1] == '"') {
                field.push_back('"');
                ++index;
            } else {
                quoted = !quoted;
            }
        } else if (character == ',' && !quoted) {
            fields.push_back(field);
            field.clear();
        } else {
            field.push_back(character);
        }
    }
    if (quoted) {
        throw std::runtime_error{"Unterminated quoted CSV field"};
    }
    fields.push_back(field);
    return fields;
}

[[nodiscard]] float parse_feature(const std::string& value, const std::size_t line_number) {
    if (value.empty() || value == "nan" || value == "NaN") {
        return std::numeric_limits<float>::quiet_NaN();
    }

    try {
        std::size_t consumed = 0;
        const auto parsed = std::stof(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument{"trailing characters"};
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error{
            "Invalid numeric feature on CSV line " + std::to_string(line_number) + ": " + value
        };
    }
}

[[nodiscard]] float parse_label(const std::string& value, const std::size_t line_number) {
    if (value == "0") {
        return 0.0F;
    }
    if (value == "1") {
        return 1.0F;
    }

    throw std::runtime_error{
        "Target must be 0 or 1 on CSV line " + std::to_string(line_number)
    };
}

[[nodiscard]] Dataset slice_rows(
    const Dataset& source,
    const std::size_t begin,
    const std::size_t end
) {
    Dataset result;
    result.feature_names = source.feature_names;
    result.dates.assign(source.dates.begin() + static_cast<std::ptrdiff_t>(begin),
                        source.dates.begin() + static_cast<std::ptrdiff_t>(end));
    result.labels.assign(source.labels.begin() + static_cast<std::ptrdiff_t>(begin),
                         source.labels.begin() + static_cast<std::ptrdiff_t>(end));

    const auto feature_begin = begin * source.feature_count();
    const auto feature_end = end * source.feature_count();
    result.features.assign(
        source.features.begin() + static_cast<std::ptrdiff_t>(feature_begin),
        source.features.begin() + static_cast<std::ptrdiff_t>(feature_end)
    );

    return result;
}

}  // namespace

std::size_t Dataset::row_count() const noexcept {
    return labels.size();
}

std::size_t Dataset::feature_count() const noexcept {
    return feature_names.size();
}

Dataset load_csv(const std::filesystem::path& path, const std::string& target_column) {
    std::ifstream input{path};
    if (!input) {
        throw std::runtime_error{"Could not open dataset: " + path.string()};
    }

    std::string header_line;
    if (!std::getline(input, header_line)) {
        throw std::runtime_error{"Dataset is empty: " + path.string()};
    }

    const auto header = split_csv_line(header_line);
    if (header.size() < 3) {
        throw std::runtime_error{"Dataset needs date, at least one feature, and target columns"};
    }

    const auto date_it = std::ranges::find(header, "date");
    const auto target_it = std::ranges::find(header, target_column);
    if (date_it == header.end()) {
        throw std::runtime_error{"CSV is missing required date column"};
    }
    if (target_it == header.end()) {
        throw std::runtime_error{"CSV is missing target column: " + target_column};
    }

    const auto date_index = static_cast<std::size_t>(std::distance(header.begin(), date_it));
    const auto target_index = static_cast<std::size_t>(std::distance(header.begin(), target_it));

    std::vector<std::size_t> feature_indices;
    Dataset dataset;
    for (std::size_t index = 0; index < header.size(); ++index) {
        if (index != date_index && index != target_index) {
            feature_indices.push_back(index);
            dataset.feature_names.push_back(header[index]);
        }
    }

    std::string line;
    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }

        const auto fields = split_csv_line(line);
        if (fields.size() != header.size()) {
            throw std::runtime_error{
                "CSV line " + std::to_string(line_number) + " has " +
                std::to_string(fields.size()) + " fields; expected " +
                std::to_string(header.size())
            };
        }

        const auto& date = fields[date_index];
        if (date.empty()) {
            throw std::runtime_error{"Empty date on CSV line " + std::to_string(line_number)};
        }
        if (!dataset.dates.empty() && date <= dataset.dates.back()) {
            throw std::runtime_error{
                "Dates must be strictly increasing; ordering error on CSV line " +
                std::to_string(line_number)
            };
        }

        dataset.dates.push_back(date);
        for (const auto index : feature_indices) {
            dataset.features.push_back(parse_feature(fields[index], line_number));
        }
        dataset.labels.push_back(parse_label(fields[target_index], line_number));
    }

    if (dataset.row_count() < 10) {
        throw std::runtime_error{"Dataset must contain at least 10 usable rows"};
    }

    return dataset;
}

DatasetSplit chronological_split(const Dataset& dataset, const double validation_fraction) {
    if (!(validation_fraction > 0.0 && validation_fraction < 0.5)) {
        throw std::invalid_argument{"Validation fraction must be greater than 0 and less than 0.5"};
    }
    if (dataset.row_count() < 10 || dataset.feature_count() == 0) {
        throw std::invalid_argument{"Dataset is too small or has no features"};
    }
    if (dataset.features.size() != dataset.row_count() * dataset.feature_count() ||
        dataset.dates.size() != dataset.row_count()) {
        throw std::invalid_argument{"Dataset dimensions are inconsistent"};
    }

    const auto validation_rows = static_cast<std::size_t>(
        std::ceil(static_cast<double>(dataset.row_count()) * validation_fraction)
    );
    const auto split_index = dataset.row_count() - validation_rows;

    return DatasetSplit{
        .train = slice_rows(dataset, 0, split_index),
        .validation = slice_rows(dataset, split_index, dataset.row_count()),
    };
}

DatasetThreeWaySplit chronological_split_by_dates(
    const Dataset& dataset,
    const std::string& train_end,
    const std::string& validation_end,
    const std::string& test_end
) {
    if (train_end.empty() || validation_end.empty() || test_end.empty() ||
        train_end >= validation_end || validation_end >= test_end) {
        throw std::invalid_argument{"Split boundaries must be non-empty and strictly increasing"};
    }
    if (dataset.row_count() < 10 || dataset.feature_count() == 0) {
        throw std::invalid_argument{"Dataset is too small or has no features"};
    }

    std::size_t train_end_index = 0;
    while (train_end_index < dataset.row_count() &&
           dataset.dates[train_end_index] <= train_end) {
        ++train_end_index;
    }
    const auto validation_begin = train_end_index;
    std::size_t validation_end_index = validation_begin;
    while (validation_end_index < dataset.row_count() &&
           dataset.dates[validation_end_index] <= validation_end) {
        ++validation_end_index;
    }
    const auto test_begin = validation_end_index;
    std::size_t test_end_index = test_begin;
    while (test_end_index < dataset.row_count() && dataset.dates[test_end_index] <= test_end) {
        ++test_end_index;
    }
    if (train_end_index == 0 || validation_end_index == validation_begin ||
        test_end_index == test_begin) {
        throw std::invalid_argument{"Requested chronological split contains an empty partition"};
    }
    if (test_end_index != dataset.row_count()) {
        throw std::invalid_argument{"Dataset contains rows after --test-end"};
    }

    return DatasetThreeWaySplit{
        .train = slice_rows(dataset, 0, train_end_index),
        .validation = slice_rows(dataset, validation_begin, validation_end_index),
        .test = slice_rows(dataset, test_begin, test_end_index),
    };
}

Dataset date_slice(const Dataset& dataset, const std::string& begin, const std::string& end) {
    if (begin.empty() || end.empty() || begin > end) {
        throw std::invalid_argument{"Date-slice boundaries must be non-empty and ordered"};
    }
    if (dataset.row_count() == 0 || dataset.feature_count() == 0 ||
        dataset.features.size() != dataset.row_count() * dataset.feature_count() ||
        dataset.dates.size() != dataset.row_count()) {
        throw std::invalid_argument{"Dataset dimensions are inconsistent for date slice"};
    }

    const auto begin_it = std::ranges::lower_bound(dataset.dates, begin);
    const auto end_it = std::ranges::upper_bound(dataset.dates, end);
    const auto begin_index = static_cast<std::size_t>(
        std::distance(dataset.dates.begin(), begin_it)
    );
    const auto end_index = static_cast<std::size_t>(
        std::distance(dataset.dates.begin(), end_it)
    );
    if (begin_index == end_index) {
        throw std::invalid_argument{
            "Requested date slice contains no rows: " + begin + " through " + end
        };
    }
    return slice_rows(dataset, begin_index, end_index);
}

Dataset row_slice(const Dataset& dataset, const std::size_t begin, const std::size_t end) {
    if (begin > end || end > dataset.row_count() ||
        dataset.features.size() != dataset.row_count() * dataset.feature_count() ||
        dataset.dates.size() != dataset.row_count()) {
        throw std::invalid_argument{"Invalid row slice or inconsistent dataset dimensions"};
    }
    if (begin == end) {
        throw std::invalid_argument{"Row slice must contain at least one row"};
    }
    return slice_rows(dataset, begin, end);
}

}  // namespace arrakis::model
