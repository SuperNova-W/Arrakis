#include "arrakis/news/feature_schema.hpp"

#include <xgboost/c_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <ranges>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

void check_xgboost(const int result, const std::string_view operation) {
    if (result != 0) {
        throw std::runtime_error{std::string{operation} + " failed: " + XGBGetLastError()};
    }
}

struct Dataset final {
    std::vector<std::string> dates;
    std::vector<std::string> feature_names;
    std::vector<float> features;
    std::vector<float> targets;

    [[nodiscard]] std::size_t row_count() const noexcept { return dates.size(); }
    [[nodiscard]] std::size_t feature_count() const noexcept { return feature_names.size(); }
};

struct PriceHistory final {
    std::vector<std::string> dates;
    std::vector<double> opens;
    std::vector<double> highs;
    std::vector<double> lows;
    std::vector<double> closes;
    std::vector<double> volumes;
    std::unordered_map<std::string, std::size_t> index_by_date;
};

struct Options final {
    std::filesystem::path input;
    std::filesystem::path market_data;
    std::filesystem::path benchmark_data;
    std::filesystem::path qqq_data;
    std::filesystem::path vix_data;
    std::filesystem::path output{"artifacts/xlk_volatility_regression.json"};
    std::string test_month_start;
    std::string test_month_end;
    int purge_sessions{1};
    int rounds{400};
    int early_stopping_rounds{30};
    int seed{42};
};

constexpr std::array<std::string_view, 12> kVolatilityFeatureNames{
    "overnight_gap_1", "intraday_range_1", "open_to_close_abs_1", "realized_vol_5",
    "realized_vol_10", "realized_vol_20", "realized_vol_60", "volume_z_20",
    "spy_realized_vol_20", "qqq_realized_vol_20", "vix_level", "vix_change_1"};

struct DMatrix final {
    explicit DMatrix(const Dataset& dataset) {
        check_xgboost(
            XGDMatrixCreateFromMat(
                dataset.features.data(),
                static_cast<bst_ulong>(dataset.row_count()),
                static_cast<bst_ulong>(dataset.feature_count()),
                std::numeric_limits<float>::quiet_NaN(),
                &handle
            ),
            "Creating regression DMatrix"
        );
        check_xgboost(
            XGDMatrixSetFloatInfo(
                handle,
                "label",
                dataset.targets.data(),
                static_cast<bst_ulong>(dataset.targets.size())
            ),
            "Setting regression labels"
        );
    }

    ~DMatrix() {
        if (handle != nullptr) XGDMatrixFree(handle);
    }
    DMatrix(const DMatrix&) = delete;
    DMatrix& operator=(const DMatrix&) = delete;
    DMatrixHandle handle{nullptr};
};

struct Booster final {
    Booster(const DMatrixHandle train, const DMatrixHandle validation) {
        const std::array<DMatrixHandle, 2> cache{train, validation};
        check_xgboost(
            XGBoosterCreate(cache.data(), static_cast<bst_ulong>(cache.size()), &handle),
            "Creating regression booster"
        );
        set_parameter("objective", "reg:squarederror");
        set_parameter("eval_metric", "rmse");
        set_parameter("tree_method", "hist");
        set_parameter("device", "cpu");
    }

    ~Booster() {
        if (handle != nullptr) XGBoosterFree(handle);
    }
    Booster(const Booster&) = delete;
    Booster& operator=(const Booster&) = delete;

    void set_parameter(const std::string_view name, const std::string_view value) {
        const std::string name_copy{name};
        const std::string value_copy{value};
        check_xgboost(
            XGBoosterSetParam(handle, name_copy.c_str(), value_copy.c_str()),
            "Setting regression parameter"
        );
    }

    void set_seed(const int seed) { set_parameter("seed", std::to_string(seed)); }

    void update(const int iteration, const DMatrixHandle train) {
        check_xgboost(XGBoosterUpdateOneIter(handle, iteration, train), "Regression update");
    }

    [[nodiscard]] double validation_qlike(
        const std::vector<float>& log_variance_targets,
        const DMatrixHandle validation
    ) const {
        constexpr double epsilon = 1.0e-10;
        const auto predictions = predict(validation);
        if (predictions.size() != log_variance_targets.size()) {
            throw std::invalid_argument{"Validation predictions and targets are misaligned"};
        }
        double total = 0.0;
        for (std::size_t index = 0; index < predictions.size(); ++index) {
            const auto actual = std::max(std::exp(static_cast<double>(log_variance_targets[index])), epsilon);
            const auto predicted = std::max(std::exp(static_cast<double>(predictions[index])), epsilon);
            const auto ratio = actual / predicted;
            total += ratio - std::log(ratio) - 1.0;
        }
        return total / static_cast<double>(predictions.size());
    }

    void save(const std::filesystem::path& path) const {
        check_xgboost(XGBoosterSaveModel(handle, path.string().c_str()), "Saving regression model");
    }

    void load(const std::filesystem::path& path) {
        check_xgboost(XGBoosterLoadModel(handle, path.string().c_str()), "Loading regression model");
    }

    [[nodiscard]] std::vector<float> predict(const DMatrixHandle matrix) const {
        constexpr std::string_view config =
            R"({"type":0,"training":false,"iteration_begin":0,"iteration_end":0,"strict_shape":true})";
        const bst_ulong* shape = nullptr;
        bst_ulong dimensions = 0;
        const float* predictions = nullptr;
        check_xgboost(
            XGBoosterPredictFromDMatrix(
                handle, matrix, config.data(), &shape, &dimensions, &predictions
            ),
            "Predicting regression"
        );
        if (dimensions != 2 || shape == nullptr || shape[1] != 1) {
            throw std::runtime_error{"Regression prediction has an invalid shape"};
        }
        return {predictions, predictions + static_cast<std::ptrdiff_t>(shape[0])};
    }

    DMatrixHandle handle{nullptr};
};

[[nodiscard]] std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const auto character = line[index];
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
    if (quoted) throw std::invalid_argument{"Unterminated quoted CSV field"};
    fields.push_back(field);
    return fields;
}

[[nodiscard]] std::string date_from_epoch(const long long timestamp) {
    const auto seconds = static_cast<std::time_t>(timestamp);
    std::tm utc{};
    if (gmtime_r(&seconds, &utc) == nullptr) throw std::runtime_error{"Invalid timestamp"};
    std::array<char, 11> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d", &utc) == 0) {
        throw std::runtime_error{"Could not format timestamp"};
    }
    return buffer.data();
}

[[nodiscard]] PriceHistory load_market(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open market history"};
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error{"Market history is empty"};
    PriceHistory result;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split_csv_line(line);
        if (fields.size() < 7) throw std::runtime_error{"Market row has too few fields"};
        const auto date = date_from_epoch(std::stoll(fields[1]));
        if (!result.dates.empty() && date <= result.dates.back()) {
            throw std::runtime_error{"Market dates are not strictly increasing"};
        }
        result.index_by_date.emplace(date, result.dates.size());
        result.dates.push_back(date);
        result.opens.push_back(std::stod(fields[2]));
        result.highs.push_back(std::stod(fields[3]));
        result.lows.push_back(std::stod(fields[4]));
        result.closes.push_back(std::stod(fields[5]));
        result.volumes.push_back(std::stod(fields[6]));
    }
    return result;
}

[[nodiscard]] PriceHistory load_vix(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open VIX history"};
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error{"VIX history is empty"};
    PriceHistory result;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split_csv_line(line);
        if (fields.size() < 2 || fields[1].empty()) continue;
        const auto date = fields[0];
        const auto value = std::stod(fields[1]);
        if (!result.dates.empty() && date <= result.dates.back()) {
            throw std::runtime_error{"VIX dates are not strictly increasing"};
        }
        result.index_by_date.emplace(date, result.dates.size());
        result.dates.push_back(date);
        result.opens.push_back(value);
        result.highs.push_back(value);
        result.lows.push_back(value);
        result.closes.push_back(value);
        result.volumes.push_back(0.0);
    }
    return result;
}

[[nodiscard]] double window_mean(
    const std::vector<double>& values,
    const std::size_t begin,
    const std::size_t end
) {
    double total = 0.0;
    for (std::size_t index = begin; index < end; ++index) total += values[index];
    return total / static_cast<double>(end - begin);
}

[[nodiscard]] double window_stddev(
    const std::vector<double>& values,
    const std::size_t begin,
    const std::size_t end
) {
    const auto mean = window_mean(values, begin, end);
    double squared = 0.0;
    for (std::size_t index = begin; index < end; ++index) {
        const auto error = values[index] - mean;
        squared += error * error;
    }
    return std::sqrt(squared / static_cast<double>(end - begin));
}

[[nodiscard]] double realized_close_volatility(
    const PriceHistory& history,
    const std::size_t index,
    const std::size_t count
) {
    if (index < count) throw std::invalid_argument{"Insufficient realized-volatility history"};
    std::vector<double> returns;
    returns.reserve(count);
    const auto begin = index - count + 1;
    for (std::size_t current = begin; current <= index; ++current) {
        returns.push_back(std::log(history.closes[current] / history.closes[current - 1]));
    }
    return window_stddev(returns, 0, returns.size());
}

[[nodiscard]] Dataset load_dataset(
    const std::filesystem::path& path,
    const PriceHistory& market,
    const PriceHistory& spy,
    const PriceHistory& qqq,
    const PriceHistory& vix
) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open feature dataset"};
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error{"Feature dataset is empty"};
    const auto header = split_csv_line(line);
    std::vector<std::size_t> feature_indices;
    for (const auto expected : arrakis::news::kMarketFeatureNames) {
        const auto found = std::ranges::find(header, expected);
        if (found == header.end()) throw std::runtime_error{"Missing market feature"};
        feature_indices.push_back(static_cast<std::size_t>(std::distance(header.begin(), found)));
    }

    Dataset result;
    for (const auto name : arrakis::news::kMarketFeatureNames) result.feature_names.emplace_back(name);
    result.feature_names.insert(
        result.feature_names.end(), kVolatilityFeatureNames.begin(), kVolatilityFeatureNames.end()
    );
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split_csv_line(line);
        if (fields.size() <= *std::ranges::max_element(feature_indices)) continue;
        const auto market_it = market.index_by_date.find(fields[0]);
        if (market_it == market.index_by_date.end()) continue;
        const auto index = market_it->second;
        const auto spy_it = spy.index_by_date.find(fields[0]);
        const auto qqq_it = qqq.index_by_date.find(fields[0]);
        const auto vix_it = vix.index_by_date.find(fields[0]);
        if (spy_it == spy.index_by_date.end() || qqq_it == qqq.index_by_date.end() ||
            vix_it == vix.index_by_date.end() || index < 60 || index == 0 ||
            index + 1 >= market.opens.size() || spy_it->second < 20 || qqq_it->second < 20 ||
            vix_it->second == 0 || !(market.opens[index] > 0.0) ||
            !(market.closes[index] > 0.0) || !(market.closes[index - 1] > 0.0)) {
            continue;
        }
        result.dates.push_back(fields[0]);
        for (const auto feature_index : feature_indices) {
            result.features.push_back(std::stof(fields[feature_index]));
        }
        const auto volume_mean = window_mean(market.volumes, index - 19, index + 1);
        const auto volume_stddev = window_stddev(market.volumes, index - 19, index + 1);
        const auto values = std::array<double, kVolatilityFeatureNames.size()>{
            market.opens[index] / market.closes[index - 1] - 1.0,
            (market.highs[index] - market.lows[index]) / market.closes[index],
            std::abs(market.closes[index] / market.opens[index] - 1.0),
            realized_close_volatility(market, index, 5),
            realized_close_volatility(market, index, 10),
            realized_close_volatility(market, index, 20),
            realized_close_volatility(market, index, 60),
            volume_stddev > 1.0e-12
                ? (market.volumes[index] - volume_mean) / volume_stddev
                : 0.0,
            realized_close_volatility(spy, spy_it->second, 20),
            realized_close_volatility(qqq, qqq_it->second, 20),
            vix.closes[vix_it->second],
            vix.closes[vix_it->second] / vix.closes[vix_it->second - 1] - 1.0,
        };
        result.features.insert(result.features.end(), values.begin(), values.end());
        const auto next_return = std::log(market.closes[index + 1] / market.opens[index + 1]);
        result.targets.push_back(static_cast<float>(std::log(next_return * next_return + 1.0e-8)));
    }
    if (result.row_count() < 100) throw std::runtime_error{"Too few regression rows"};
    return result;
}

[[nodiscard]] Dataset row_slice(const Dataset& source, const std::size_t begin, const std::size_t end) {
    if (begin >= end || end > source.row_count()) throw std::invalid_argument{"Invalid regression slice"};
    Dataset result;
    result.feature_names = source.feature_names;
    result.dates.assign(source.dates.begin() + static_cast<std::ptrdiff_t>(begin), source.dates.begin() + static_cast<std::ptrdiff_t>(end));
    result.targets.assign(source.targets.begin() + static_cast<std::ptrdiff_t>(begin), source.targets.begin() + static_cast<std::ptrdiff_t>(end));
    const auto feature_begin = begin * source.feature_count();
    const auto feature_end = end * source.feature_count();
    result.features.assign(source.features.begin() + static_cast<std::ptrdiff_t>(feature_begin), source.features.begin() + static_cast<std::ptrdiff_t>(feature_end));
    return result;
}

struct MonthRange final {
    std::string month;
    std::size_t begin{};
    std::size_t end{};
};

[[nodiscard]] std::vector<MonthRange> month_ranges(const Dataset& dataset) {
    std::vector<MonthRange> result;
    for (std::size_t index = 0; index < dataset.row_count(); ++index) {
        const auto month = dataset.dates[index].substr(0, 7);
        if (result.empty() || result.back().month != month) {
            result.push_back(MonthRange{.month = month, .begin = index, .end = index + 1});
        } else {
            result.back().end = index + 1;
        }
    }
    return result;
}

[[nodiscard]] double rolling_variance(
    const PriceHistory& market,
    const std::size_t index,
    const std::size_t count
) {
    if (index < count) throw std::invalid_argument{"Insufficient volatility history"};
    double sum = 0.0;
    const auto begin = index - count + 1;
    for (std::size_t current = begin; current <= index; ++current) {
        const auto value = std::log(market.closes[current] / market.opens[current]);
        sum += value * value;
    }
    return sum / static_cast<double>(count);
}

[[nodiscard]] double ewma_variance(const PriceHistory& market, const std::size_t index) {
    constexpr double alpha = 2.0 / 21.0;
    double variance = 0.0;
    for (std::size_t current = 1; current <= index; ++current) {
        const auto value = std::log(market.closes[current] / market.opens[current]);
        variance = (1.0 - alpha) * variance + alpha * value * value;
    }
    return variance;
}

struct AggregateMetrics final {
    double qlike{};
    double mae_log_variance{};
    double rmse_log_variance{};
    double spearman{};
    double variance_ratio{};
};

[[nodiscard]] std::vector<double> ranks(const std::vector<double>& values) {
    std::vector<std::pair<double, std::size_t>> ordered;
    ordered.reserve(values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        ordered.emplace_back(values[index], index);
    }
    std::ranges::sort(ordered);
    std::vector<double> output(values.size());
    std::size_t index = 0;
    while (index < ordered.size()) {
        auto end = index + 1;
        while (end < ordered.size() && ordered[end].first == ordered[index].first) ++end;
        const auto rank = (static_cast<double>(index + 1) + static_cast<double>(end)) / 2.0;
        for (std::size_t current = index; current < end; ++current) {
            output[ordered[current].second] = rank;
        }
        index = end;
    }
    return output;
}

[[nodiscard]] AggregateMetrics evaluate(
    const std::vector<float>& actual_variances,
    const std::vector<double>& predictions
) {
    if (actual_variances.size() != predictions.size() || actual_variances.empty()) {
        throw std::invalid_argument{"Regression metrics are misaligned"};
    }
    constexpr double epsilon = 1.0e-10;
    double qlike_sum = 0.0;
    double mae_log_sum = 0.0;
    double squared_log_sum = 0.0;
    double actual_mean = 0.0;
    double prediction_mean = 0.0;
    for (std::size_t index = 0; index < actual_variances.size(); ++index) {
        const auto actual = std::max(static_cast<double>(actual_variances[index]), epsilon);
        const auto prediction = std::max(predictions[index], epsilon);
        const auto ratio = actual / prediction;
        qlike_sum += ratio - std::log(ratio) - 1.0;
        const auto log_error = std::log(prediction) - std::log(actual);
        mae_log_sum += std::abs(log_error);
        squared_log_sum += log_error * log_error;
        actual_mean += actual;
        prediction_mean += prediction;
    }
    const auto count = static_cast<double>(actual_variances.size());
    actual_mean /= count;
    prediction_mean /= count;
    std::vector<double> actual_values;
    std::vector<double> predicted_values;
    actual_values.reserve(actual_variances.size());
    predicted_values.reserve(predictions.size());
    for (std::size_t index = 0; index < actual_variances.size(); ++index) {
        actual_values.push_back(static_cast<double>(actual_variances[index]));
        predicted_values.push_back(std::max(predictions[index], epsilon));
    }
    const auto actual_ranks = ranks(actual_values);
    const auto predicted_ranks = ranks(predicted_values);
    const auto actual_rank_mean = (static_cast<double>(actual_ranks.size()) + 1.0) / 2.0;
    const auto predicted_rank_mean = actual_rank_mean;
    double covariance = 0.0;
    double actual_squared = 0.0;
    double prediction_squared = 0.0;
    for (std::size_t index = 0; index < actual_ranks.size(); ++index) {
        const auto actual_error = actual_ranks[index] - actual_rank_mean;
        const auto prediction_error = predicted_ranks[index] - predicted_rank_mean;
        covariance += actual_error * prediction_error;
        actual_squared += actual_error * actual_error;
        prediction_squared += prediction_error * prediction_error;
    }
    return AggregateMetrics{
        .qlike = qlike_sum / count,
        .mae_log_variance = mae_log_sum / count,
        .rmse_log_variance = std::sqrt(squared_log_sum / count),
        .spearman = actual_squared > 1.0e-20 && prediction_squared > 1.0e-20
                        ? covariance / std::sqrt(actual_squared * prediction_squared)
                        : 0.0,
        .variance_ratio = prediction_mean / actual_mean,
    };
}

[[nodiscard]] double paired_block_bootstrap_lower_bound(
    const std::vector<double>& differences,
    const std::size_t block_length,
    const std::size_t samples,
    const unsigned int seed
) {
    if (differences.empty() || block_length == 0 || samples == 0) {
        throw std::invalid_argument{"Invalid paired bootstrap input"};
    }
    std::mt19937 generator{seed};
    std::uniform_int_distribution<std::size_t> start_distribution(
        0, differences.size() - 1
    );
    std::vector<double> means;
    means.reserve(samples);
    for (std::size_t sample = 0; sample < samples; ++sample) {
        double total = 0.0;
        std::size_t count = 0;
        while (count < differences.size()) {
            const auto start = start_distribution(generator);
            for (std::size_t offset = 0; offset < block_length && count < differences.size(); ++offset) {
                total += differences[(start + offset) % differences.size()];
                ++count;
            }
        }
        means.push_back(total / static_cast<double>(differences.size()));
    }
    std::ranges::sort(means);
    const auto index = static_cast<std::size_t>(
        std::floor(0.025 * static_cast<double>(means.size() - 1))
    );
    return means[index];
}

[[nodiscard]] Options parse_options(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto require_value = [&]() -> std::string_view {
            if (index + 1 >= argc) throw std::invalid_argument{"Missing option value"};
            ++index;
            return argv[index];
        };
        if (argument == "--input") options.input = require_value();
        else if (argument == "--market-data") options.market_data = require_value();
        else if (argument == "--benchmark-data") options.benchmark_data = require_value();
        else if (argument == "--qqq-data") options.qqq_data = require_value();
        else if (argument == "--vix-data") options.vix_data = require_value();
        else if (argument == "--output") options.output = require_value();
        else if (argument == "--test-month-start") options.test_month_start = require_value();
        else if (argument == "--test-month-end") options.test_month_end = require_value();
        else if (argument == "--purge-sessions") options.purge_sessions = std::stoi(std::string{require_value()});
        else if (argument == "--rounds") options.rounds = std::stoi(std::string{require_value()});
        else if (argument == "--early-stopping-rounds") options.early_stopping_rounds = std::stoi(std::string{require_value()});
        else if (argument == "--seed") options.seed = std::stoi(std::string{require_value()});
        else if (argument == "--help") {
            std::cout << "Usage: arrakis-train-volatility-regression --input <csv> --market-data <csv> "
                         "--benchmark-data <csv> --qqq-data <csv> --vix-data <csv> "
                         "--output <json> [--test-month-start YYYY-MM --test-month-end YYYY-MM]\n";
            std::exit(0);
        } else throw std::invalid_argument{"Unknown argument: " + std::string{argument}};
    }
    if (options.input.empty() || options.market_data.empty() || options.benchmark_data.empty() ||
        options.qqq_data.empty() || options.vix_data.empty()) {
        throw std::invalid_argument{
            "--input, --market-data, --benchmark-data, --qqq-data, and --vix-data are required"
        };
    }
    if (options.rounds <= 0 || options.early_stopping_rounds < 0 || options.purge_sessions < 0) {
        throw std::invalid_argument{"Invalid regression training option"};
    }
    return options;
}

void write_metric(std::ostream& output, const AggregateMetrics& metrics) {
    output << "{\"qlike\": " << metrics.qlike << ", \"mae_log_variance\": "
           << metrics.mae_log_variance << ", \"rmse_log_variance\": "
           << metrics.rmse_log_variance << ", \"spearman\": " << metrics.spearman
           << ", \"variance_ratio\": " << metrics.variance_ratio << "}";
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto market = load_market(options.market_data);
        const auto spy = load_market(options.benchmark_data);
        const auto qqq = load_market(options.qqq_data);
        const auto vix = load_vix(options.vix_data);
        const auto dataset = load_dataset(options.input, market, spy, qqq, vix);
        const auto ranges = month_ranges(dataset);
        if (ranges.size() < 12) throw std::runtime_error{"Need at least 12 months"};

        std::vector<std::string> prediction_dates;
        std::vector<float> actual_variances;
        std::vector<double> model_predictions;
        std::vector<double> rolling_predictions;
        std::vector<double> ewma_predictions;
        std::vector<double> previous_predictions;
        std::vector<double> qlike_differences;
        std::vector<AggregateMetrics> model_fold_metrics;
        std::vector<AggregateMetrics> ewma_fold_metrics;
        const auto first_year = std::stoi(ranges.front().month.substr(0, 4));

        for (std::size_t test_index = 0; test_index < ranges.size(); ++test_index) {
            const auto& test_range = ranges[test_index];
            if (test_index < 6 || std::stoi(test_range.month.substr(0, 4)) < first_year + 2 ||
                (!options.test_month_start.empty() && test_range.month < options.test_month_start) ||
                (!options.test_month_end.empty() && test_range.month > options.test_month_end)) {
                continue;
            }
            const auto& validation_range = ranges[test_index - 6];
            const auto purge = static_cast<std::size_t>(options.purge_sessions);
            if (validation_range.begin <= purge || test_range.begin <= purge) continue;
            const auto train_end = validation_range.begin - purge;
            const auto validation_end = test_range.begin - purge;
            const auto train = row_slice(dataset, 0, train_end);
            const auto validation = row_slice(dataset, validation_range.begin, validation_end);
            const auto test = row_slice(dataset, test_range.begin, test_range.end);
            const auto checkpoint = options.output.string() + "." + test_range.month + ".tmp.ubj";

            const DMatrix train_matrix{train};
            const DMatrix validation_matrix{validation};
            const DMatrix test_matrix{test};
            Booster booster{train_matrix.handle, validation_matrix.handle};
            booster.set_parameter("max_depth", "2");
            booster.set_parameter("eta", "0.03");
            booster.set_parameter("min_child_weight", "10");
            booster.set_parameter("subsample", "0.8");
            booster.set_parameter("colsample_bytree", "0.8");
            booster.set_parameter("lambda", "10");
            booster.set_seed(options.seed);
            double best_qlike = std::numeric_limits<double>::infinity();
            int no_improvement = 0;
            for (int iteration = 0; iteration < options.rounds; ++iteration) {
                booster.update(iteration, train_matrix.handle);
                const auto qlike = booster.validation_qlike(
                    validation.targets, validation_matrix.handle
                );
                if (qlike + 1.0e-12 < best_qlike) {
                    best_qlike = qlike;
                    no_improvement = 0;
                    booster.save(checkpoint);
                } else {
                    ++no_improvement;
                    if (no_improvement >= options.early_stopping_rounds) break;
                }
            }
            Booster best_booster{train_matrix.handle, validation_matrix.handle};
            best_booster.load(checkpoint);
            std::error_code error;
            std::filesystem::remove(checkpoint, error);
            const auto validation_predictions = best_booster.predict(validation_matrix.handle);
            const auto predictions = best_booster.predict(test_matrix.handle);
            double scale_sum = 0.0;
            for (std::size_t index = 0; index < validation_predictions.size(); ++index) {
                const auto actual = std::exp(static_cast<double>(validation.targets[index]));
                const auto predicted = std::exp(static_cast<double>(validation_predictions[index]));
                scale_sum += actual / std::max(predicted, 1.0e-10);
            }
            const auto scale = scale_sum / static_cast<double>(validation_predictions.size());
            std::vector<float> fold_actual_variances;
            std::vector<double> fold_model_predictions;
            std::vector<double> fold_ewma_predictions;
            fold_actual_variances.reserve(test.row_count());
            fold_model_predictions.reserve(test.row_count());
            fold_ewma_predictions.reserve(test.row_count());
            for (std::size_t row = 0; row < test.row_count(); ++row) {
                const auto market_index = market.index_by_date.at(test.dates[row]);
                prediction_dates.push_back(test.dates[row]);
                actual_variances.push_back(
                    static_cast<float>(std::exp(static_cast<double>(test.targets[row])))
                );
                model_predictions.push_back(
                    std::max(std::exp(static_cast<double>(predictions[row])) * scale, 1.0e-10)
                );
                const auto rolling = rolling_variance(market, market_index, 20);
                const auto ewma = ewma_variance(market, market_index);
                rolling_predictions.push_back(rolling);
                ewma_predictions.push_back(ewma);
                fold_actual_variances.push_back(actual_variances.back());
                fold_model_predictions.push_back(model_predictions.back());
                fold_ewma_predictions.push_back(ewma);
                const auto actual = std::max(static_cast<double>(actual_variances.back()), 1.0e-10);
                const auto model = std::max(model_predictions.back(), 1.0e-10);
                const auto ratio = actual / model;
                const auto model_qlike = ratio - std::log(ratio) - 1.0;
                const auto ewma_ratio = actual / std::max(ewma, 1.0e-10);
                const auto ewma_qlike = ewma_ratio - std::log(ewma_ratio) - 1.0;
                qlike_differences.push_back(ewma_qlike - model_qlike);
                const auto current_return = std::log(
                    market.closes[market_index] / market.opens[market_index]
                );
                previous_predictions.push_back(std::max(current_return * current_return, 1.0e-10));
            }
            model_fold_metrics.push_back(evaluate(fold_actual_variances, fold_model_predictions));
            ewma_fold_metrics.push_back(evaluate(fold_actual_variances, fold_ewma_predictions));
        }

        if (actual_variances.empty()) throw std::runtime_error{"No regression test folds"};
        if (options.output.has_parent_path()) std::filesystem::create_directories(options.output.parent_path());
        std::ofstream predictions_file{options.output.string() + ".oos_predictions.csv"};
        if (!predictions_file) throw std::runtime_error{"Could not write regression predictions"};
        predictions_file << "date,actual_variance,model_variance,rolling_variance,ewma_variance,previous_variance\n";
        for (std::size_t index = 0; index < prediction_dates.size(); ++index) {
            predictions_file << prediction_dates[index] << ',' << actual_variances[index] << ','
                             << model_predictions[index] << ',' << rolling_predictions[index] << ','
                             << ewma_predictions[index] << ',' << previous_predictions[index] << '\n';
        }

        const auto model_metrics = evaluate(actual_variances, model_predictions);
        const auto rolling_metrics = evaluate(actual_variances, rolling_predictions);
        const auto ewma_metrics = evaluate(actual_variances, ewma_predictions);
        const auto previous_metrics = evaluate(actual_variances, previous_predictions);
        const auto mean_qlike_improvement = ewma_metrics.qlike - model_metrics.qlike;
        const auto bootstrap_lower = paired_block_bootstrap_lower_bound(
            qlike_differences, 20, 1000, static_cast<unsigned int>(options.seed)
        );
        const auto favorable_folds = static_cast<std::size_t>(std::ranges::count_if(
            model_fold_metrics,
            [&ewma_fold_metrics, index = std::size_t{0}](const auto& metric) mutable {
                const auto favorable = metric.qlike < ewma_fold_metrics[index].qlike;
                ++index;
                return favorable;
            }
        ));
        std::ofstream output{options.output};
        if (!output) throw std::runtime_error{"Could not write regression report"};
        output << std::fixed << std::setprecision(8)
               << "{\n  \"protocol\": {\n"
               << "    \"target\": \"log(next-session squared open-to-close return + 1e-8)\",\n"
               << "    \"feature_subset\": \"market-context-volatility\",\n"
               << "    \"validation\": \"trailing six months, one-session purge, early stopping on validation QLIKE\",\n"
               << "    \"scale_calibration\": \"fold-local mean(realized_variance / exp(validation_log_prediction))\",\n"
               << "    \"xgboost\": \"reg:squarederror on log variance, depth2, eta0.03, min_child10, lambda10, subsample0.8, colsample0.8\",\n"
               << "    \"test_month_start\": \"" << options.test_month_start << "\",\n"
               << "    \"test_month_end\": \"" << options.test_month_end << "\"\n  },\n"
               << "  \"rows\": " << actual_variances.size() << ",\n"
               << "  \"qlike_improvement_over_ewma\": " << mean_qlike_improvement << ",\n"
               << "  \"paired_block_bootstrap_lower_2_5_percentile\": " << bootstrap_lower << ",\n"
               << "  \"favorable_months\": " << favorable_folds << ",\n"
               << "  \"total_months\": " << model_fold_metrics.size() << ",\n"
               << "  \"metrics\": {\n    \"model\": ";
        write_metric(output, model_metrics);
        output << ",\n    \"rolling_20_intraday_variance\": ";
        write_metric(output, rolling_metrics);
        output << ",\n    \"ewma_half_life_20\": ";
        write_metric(output, ewma_metrics);
        output << ",\n    \"previous_intraday_variance\": ";
        write_metric(output, previous_metrics);
        output << "\n  }\n}\n";

        std::cout << "Volatility regression rows=" << actual_variances.size()
                  << ", model QLIKE=" << model_metrics.qlike
                  << ", EWMA QLIKE=" << ewma_metrics.qlike
                  << ", improvement=" << mean_qlike_improvement
                  << ", bootstrap lower=" << bootstrap_lower
                  << ", favorable folds=" << favorable_folds << "/"
                  << model_fold_metrics.size() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-train-volatility-regression: " << error.what() << '\n';
        return 1;
    }
}
