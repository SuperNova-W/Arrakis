#include "arrakis/model/dataset.hpp"
#include "arrakis/model/metrics.hpp"
#include "arrakis/news/feature_schema.hpp"

#include <openssl/evp.h>
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
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

void check_xgboost(const int result, const std::string_view operation) {
    if (result != 0) {
        throw std::runtime_error{
            std::string{operation} + " failed: " + XGBGetLastError()
        };
    }
}

class DMatrix final {
  public:
    explicit DMatrix(const arrakis::model::Dataset& dataset) {
        check_xgboost(
            XGDMatrixCreateFromMat(
                dataset.features.data(),
                static_cast<bst_ulong>(dataset.row_count()),
                static_cast<bst_ulong>(dataset.feature_count()),
                std::numeric_limits<float>::quiet_NaN(),
                &handle_
            ),
            "Creating DMatrix"
        );
        check_xgboost(
            XGDMatrixSetFloatInfo(
                handle_,
                "label",
                dataset.labels.data(),
                static_cast<bst_ulong>(dataset.labels.size())
            ),
            "Setting labels"
        );
    }

    ~DMatrix() {
        if (handle_ != nullptr) {
            XGDMatrixFree(handle_);
        }
    }

    DMatrix(const DMatrix&) = delete;
    DMatrix& operator=(const DMatrix&) = delete;
    DMatrix(DMatrix&&) = delete;
    DMatrix& operator=(DMatrix&&) = delete;

    [[nodiscard]] DMatrixHandle get() const noexcept { return handle_; }

  private:
    DMatrixHandle handle_{nullptr};
};

class Booster final {
  public:
    explicit Booster(const std::vector<DMatrixHandle>& cache) {
        check_xgboost(
            XGBoosterCreate(cache.data(), static_cast<bst_ulong>(cache.size()), &handle_),
            "Creating booster"
        );
    }

    ~Booster() {
        if (handle_ != nullptr) {
            XGBoosterFree(handle_);
        }
    }

    Booster(const Booster&) = delete;
    Booster& operator=(const Booster&) = delete;
    Booster(Booster&&) = delete;
    Booster& operator=(Booster&&) = delete;

    void set_parameter(const std::string_view name, const std::string_view value) {
        const std::string name_copy{name};
        const std::string value_copy{value};
        check_xgboost(
            XGBoosterSetParam(handle_, name_copy.c_str(), value_copy.c_str()),
            "Setting parameter " + name_copy
        );
    }

    void update(const int iteration, const DMatrixHandle train) {
        check_xgboost(
            XGBoosterUpdateOneIter(handle_, iteration, train),
            "Training iteration " + std::to_string(iteration)
        );
    }

    [[nodiscard]] std::string evaluate(
        const int iteration,
        std::vector<DMatrixHandle> matrices,
        std::vector<const char*> names
    ) const {
        const char* result = nullptr;
        check_xgboost(
            XGBoosterEvalOneIter(
                handle_,
                iteration,
                matrices.data(),
                names.data(),
                static_cast<bst_ulong>(matrices.size()),
                &result
            ),
            "Evaluating model"
        );
        return result;
    }

    [[nodiscard]] std::vector<float> predict(const DMatrixHandle matrix) const {
        constexpr std::string_view config =
            R"({"type":0,"training":false,"iteration_begin":0,"iteration_end":0,"strict_shape":true})";
        const bst_ulong* shape = nullptr;
        bst_ulong dimensions = 0;
        const float* predictions = nullptr;

        check_xgboost(
            XGBoosterPredictFromDMatrix(
                handle_, matrix, config.data(), &shape, &dimensions, &predictions
            ),
            "Predicting data"
        );
        if (dimensions == 0 || shape == nullptr) {
            throw std::runtime_error{"XGBoost returned an invalid prediction shape"};
        }

        std::size_t prediction_count = 1;
        for (bst_ulong index = 0; index < dimensions; ++index) {
            prediction_count *= static_cast<std::size_t>(shape[index]);
        }
        return {predictions, predictions + prediction_count};
    }

    void save(const std::filesystem::path& path) const {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
        check_xgboost(XGBoosterSaveModel(handle_, path.string().c_str()), "Saving model");
    }

    void load(const std::filesystem::path& path) {
        check_xgboost(XGBoosterLoadModel(handle_, path.string().c_str()), "Loading model");
    }

  private:
    BoosterHandle handle_{nullptr};
};

struct Hyperparameters final {
    double eta{0.05};
    int max_depth{3};
    int min_child_weight{1};
    double subsample{0.8};
    double colsample_bytree{0.8};
    double lambda{1.0};
    double alpha{0.0};
    int seed{42};
};

struct Options final {
    std::filesystem::path input;
    std::filesystem::path model_output{"artifacts/xlk_news_xgboost.json"};
    std::filesystem::path walk_forward_output{"artifacts/xlk_walk_forward.json"};
    std::filesystem::path monthly_walk_forward_output{
        "artifacts/xlk_monthly_walk_forward.json"
    };
    std::filesystem::path ablation_output{"artifacts/xlk_ablation.json"};
    std::string target{"target_next_close_up"};
    std::string feature_subset{"combined"};
    std::filesystem::path market_data;
    std::filesystem::path benchmark_data;
    double validation_fraction{0.2};
    int rounds{75};
    int early_stopping_rounds{20};
    std::string train_end;
    std::string validation_end;
    std::string test_end;
    Hyperparameters hyperparameters;
    bool hyperparameter_search{false};
    bool walk_forward{false};
    bool monthly_walk_forward{false};
    bool ablation{false};
    bool volatility_features{false};
    bool context_volatility_features{false};
    std::filesystem::path qqq_data;
    std::filesystem::path vix_data;
    int purge_sessions{1};
    std::string test_month_start;
    std::string test_month_end;
    std::size_t train_window_months{};
};

struct TrainingResult final {
    int best_iteration_index{-1};
    int rounds_run{0};
    double best_validation_logloss{std::numeric_limits<double>::infinity()};
};

struct SearchResult final {
    Hyperparameters hyperparameters;
    double validation_logloss{};
    int best_iteration{};
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

struct TargetSpec final {
    int horizon{};
    bool excess_return{false};
    bool high_volatility{false};
    bool open_to_close{false};
};

constexpr std::array<std::string_view, 7> kWalkForwardTargets{
    "target_next_close_up",
    "forward_return_3d_up",
    "forward_return_5d_up",
    "excess_return_3d_up",
    "excess_return_5d_up",
    "forward_return_5d_open_to_close_up",
    "target_high_volatility_next_day",
};

constexpr std::array<std::string_view, 9> kVolatilityFeatureNames{
    "overnight_gap_1", "intraday_range_1", "open_to_close_abs_1", "realized_vol_5",
    "realized_vol_10", "realized_vol_20", "realized_vol_60", "volume_z_20",
    "spy_realized_vol_20"};

constexpr std::array<std::string_view, 18> kMarketVolatilityFeatureNames{
    "ret_1", "ret_3", "ret_6", "volatility_6", "volume_mean_6", "rel_volume",
    "rsi_14", "spy_ret_1", "sector_spy_diff", "overnight_gap_1", "intraday_range_1",
    "open_to_close_abs_1", "realized_vol_5", "realized_vol_10", "realized_vol_20",
    "realized_vol_60", "volume_z_20", "spy_realized_vol_20"};

constexpr std::array<std::string_view, 3> kContextVolatilityFeatureNames{
    "qqq_realized_vol_20", "vix_level", "vix_change_1"};

constexpr std::array<std::string_view, 21> kMarketContextVolatilityFeatureNames{
    "ret_1", "ret_3", "ret_6", "volatility_6", "volume_mean_6", "rel_volume",
    "rsi_14", "spy_ret_1", "sector_spy_diff", "overnight_gap_1", "intraday_range_1",
    "open_to_close_abs_1", "realized_vol_5", "realized_vol_10", "realized_vol_20",
    "realized_vol_60", "volume_z_20", "spy_realized_vol_20", "qqq_realized_vol_20",
    "vix_level", "vix_change_1"};

struct WalkForwardWindowResult final {
    int test_year{};
    std::size_t train_rows{};
    std::size_t validation_rows{};
    std::size_t test_rows{};
    std::string train_start;
    std::string train_end;
    std::string validation_start;
    std::string validation_end;
    std::string test_start;
    std::string test_end;
    int selected_best_iteration{};
    std::optional<arrakis::model::BinaryMetrics> validation_metrics;
    std::optional<arrakis::model::BinaryMetrics> test_metrics;
};

struct WalkForwardTargetResult final {
    std::string target;
    std::vector<WalkForwardWindowResult> windows;
    bool promotion_candidate{};
};

struct MonthlyWindowResult final {
    std::string test_month;
    std::size_t train_rows{};
    std::size_t validation_rows{};
    std::size_t test_rows{};
    std::string train_start;
    std::string train_end;
    std::string validation_start;
    std::string validation_end;
    std::string test_start;
    std::string test_end;
    int selected_best_iteration{};
    std::optional<arrakis::model::BinaryMetrics> validation_metrics;
    std::optional<arrakis::model::BinaryMetrics> test_metrics;
    std::optional<arrakis::model::BinaryMetrics> calibrated_test_metrics;
};

struct MonthlyEvaluationResult final {
    std::string feature_subset;
    std::vector<MonthlyWindowResult> windows;
    std::vector<std::string> prediction_dates;
    std::vector<float> prediction_labels;
    std::vector<float> prediction_probabilities;
    std::vector<float> calibrated_prediction_probabilities;
};

struct YearMetrics final {
    int year{};
    std::size_t rows{};
    std::optional<arrakis::model::BinaryMetrics> metrics;
};

struct ConfusionMatrix final {
    std::size_t true_negative{};
    std::size_t false_positive{};
    std::size_t false_negative{};
    std::size_t true_positive{};
};

struct PredictionDiagnostics final {
    double minimum{};
    double maximum{};
    double mean{};
    double standard_deviation{};
    std::array<std::size_t, 10> histogram{};
    ConfusionMatrix confusion_matrix;
    std::size_t positive_labels{};
    std::size_t negative_labels{};
    int majority_label{};
    double majority_accuracy{};

    [[nodiscard]] bool is_constant() const noexcept {
        return maximum - minimum < 1.0e-3 && standard_deviation < 1.0e-4;
    }
};

[[nodiscard]] std::string format_double(const double value) {
    std::ostringstream output;
    output << std::setprecision(12) << value;
    return output.str();
}

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

[[nodiscard]] std::string date_from_epoch(const long long timestamp) {
    const auto seconds = static_cast<std::time_t>(timestamp);
    std::tm utc{};
    if (gmtime_r(&seconds, &utc) == nullptr) {
        throw std::runtime_error{"Could not convert market timestamp to a date"};
    }
    std::array<char, 11> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d", &utc) == 0) {
        throw std::runtime_error{"Could not format market timestamp date"};
    }
    return buffer.data();
}

[[nodiscard]] PriceHistory load_price_history(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) {
        throw std::runtime_error{"Could not open market history: " + path.string()};
    }

    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error{"Market history is empty: " + path.string()};
    }

    PriceHistory history;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const auto fields = split_csv_line(line);
        if (fields.size() < 6) {
            throw std::runtime_error{"Market history row has too few fields: " + path.string()};
        }
        const auto date = date_from_epoch(std::stoll(fields[1]));
        if (!history.dates.empty() && date <= history.dates.back()) {
            throw std::runtime_error{"Market history dates must be strictly increasing"};
        }
        history.index_by_date.emplace(date, history.dates.size());
        history.dates.push_back(date);
        history.opens.push_back(std::stod(fields[2]));
        history.highs.push_back(std::stod(fields[3]));
        history.lows.push_back(std::stod(fields[4]));
        history.closes.push_back(std::stod(fields[5]));
        history.volumes.push_back(std::stod(fields[6]));
    }

    if (history.dates.empty()) {
        throw std::runtime_error{"Market history has no usable rows: " + path.string()};
    }
    return history;
}

[[nodiscard]] PriceHistory load_vix_history(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open VIX history: " + path.string()};
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error{"VIX history is empty"};

    PriceHistory history;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split_csv_line(line);
        if (fields.size() < 2 || fields[1].empty()) continue;
        const auto date = fields[0];
        if (!history.dates.empty() && date <= history.dates.back()) {
            throw std::runtime_error{"VIX dates must be strictly increasing"};
        }
        const auto close = std::stod(fields[1]);
        if (!(close > 0.0)) throw std::runtime_error{"VIX history contains a non-positive value"};
        history.index_by_date.emplace(date, history.dates.size());
        history.dates.push_back(date);
        history.opens.push_back(close);
        history.highs.push_back(close);
        history.lows.push_back(close);
        history.closes.push_back(close);
        history.volumes.push_back(0.0);
    }
    if (history.dates.empty()) throw std::runtime_error{"VIX history has no usable rows"};
    return history;
}

[[nodiscard]] std::optional<TargetSpec> alternative_target_spec(const std::string_view target) {
    if (target == "target_high_volatility_next_day") {
        return TargetSpec{.horizon = 1, .excess_return = false, .high_volatility = true};
    }
    if (target == "forward_return_3d_up") {
        return TargetSpec{.horizon = 3, .excess_return = false};
    }
    if (target == "forward_return_5d_up") {
        return TargetSpec{.horizon = 5, .excess_return = false};
    }
    if (target == "forward_return_5d_open_to_close_up") {
        return TargetSpec{.horizon = 5, .excess_return = false, .open_to_close = true};
    }
    if (target == "forward_return_1d_open_to_close_up") {
        return TargetSpec{.horizon = 1, .excess_return = false, .open_to_close = true};
    }
    if (target == "excess_return_3d_up") {
        return TargetSpec{.horizon = 3, .excess_return = true};
    }
    if (target == "excess_return_5d_up") {
        return TargetSpec{.horizon = 5, .excess_return = true};
    }
    return std::nullopt;
}

[[nodiscard]] double trailing_median_absolute_return(
    const PriceHistory& market,
    const std::size_t current_index
) {
    constexpr std::size_t trailing_return_count = 20;
    if (current_index < trailing_return_count) {
        throw std::invalid_argument{"Volatility target requires 20 trailing returns"};
    }

    std::array<double, trailing_return_count> absolute_returns{};
    const auto first_return_index = current_index - trailing_return_count + 1;
    for (std::size_t offset = 0; offset < trailing_return_count; ++offset) {
        const auto return_index = first_return_index + offset;
        absolute_returns[offset] = std::abs(
            market.closes[return_index] / market.opens[return_index] - 1.0
        );
    }
    std::ranges::sort(absolute_returns);
    return (absolute_returns[trailing_return_count / 2 - 1] +
            absolute_returns[trailing_return_count / 2]) /
           2.0;
}

[[nodiscard]] arrakis::model::Dataset apply_alternative_target(
    const arrakis::model::Dataset& source,
    const TargetSpec spec,
    const PriceHistory& market,
    const std::optional<PriceHistory>& benchmark
) {
    if (spec.excess_return && !benchmark.has_value()) {
        throw std::invalid_argument{
            "Excess-return targets require --benchmark-data"
        };
    }

    arrakis::model::Dataset result;
    result.feature_names = source.feature_names;
    result.dates.reserve(source.row_count());
    result.labels.reserve(source.row_count());
    result.features.reserve(source.features.size());

    for (std::size_t row = 0; row < source.row_count(); ++row) {
        const auto market_it = market.index_by_date.find(source.dates[row]);
        if (market_it == market.index_by_date.end()) {
            continue;
        }
        const auto future_index = market_it->second + static_cast<std::size_t>(spec.horizon);
        if (future_index >= market.closes.size()) {
            continue;
        }

        const auto start_index = market_it->second + (spec.open_to_close ? 1U : 0U);
        if (start_index >= market.opens.size() || !(market.opens[start_index] > 0.0)) {
            continue;
        }
        double forward_return = market.closes[future_index] /
                                (spec.open_to_close ? market.opens[start_index]
                                                     : market.closes[market_it->second]) -
                                1.0;
        if (spec.excess_return) {
            const auto& benchmark_history = benchmark.value();
            const auto benchmark_it =
                benchmark_history.index_by_date.find(source.dates[row]);
            const auto benchmark_future_it = benchmark_history.index_by_date.find(
                market.dates[future_index]
            );
            if (benchmark_it == benchmark_history.index_by_date.end() ||
                benchmark_future_it == benchmark_history.index_by_date.end()) {
                continue;
            }
            forward_return -= benchmark_history.closes[benchmark_future_it->second] /
                              benchmark_history.closes[benchmark_it->second] - 1.0;
        }

        bool positive_target = forward_return > 0.0;
        if (spec.high_volatility) {
            if (market_it->second < 20) {
                continue;
            }
            const auto next_session_index = market_it->second + 1;
            if (next_session_index >= market.closes.size() ||
                next_session_index >= market.opens.size()) {
                continue;
            }
            const auto threshold = trailing_median_absolute_return(market, market_it->second);
            const auto next_open_to_close_return =
                market.closes[next_session_index] / market.opens[next_session_index] - 1.0;
            positive_target = std::abs(next_open_to_close_return) > threshold;
        }

        result.dates.push_back(source.dates[row]);
        result.labels.push_back(positive_target ? 1.0F : 0.0F);
        const auto feature_begin = row * source.feature_count();
        const auto feature_end = feature_begin + source.feature_count();
        result.features.insert(
            result.features.end(),
            source.features.begin() + static_cast<std::ptrdiff_t>(feature_begin),
            source.features.begin() + static_cast<std::ptrdiff_t>(feature_end)
        );
    }

    if (result.row_count() < 10) {
        throw std::runtime_error{"Alternative target produced too few aligned rows"};
    }
    return result;
}

[[nodiscard]] arrakis::model::Dataset load_target_dataset(const Options& options) {
    const auto alternative = alternative_target_spec(options.target);
    auto dataset = arrakis::model::load_csv(options.input, "target_next_close_up");
    if (!alternative.has_value()) return dataset;

    const auto market = load_price_history(options.market_data);
    std::optional<PriceHistory> benchmark;
    if (alternative->excess_return) {
        benchmark = load_price_history(options.benchmark_data);
    }
    return apply_alternative_target(dataset, *alternative, market, benchmark);
}

[[nodiscard]] double window_mean(
    const std::vector<double>& values,
    const std::size_t begin,
    const std::size_t end
) {
    if (begin >= end || end > values.size()) {
        throw std::invalid_argument{"Invalid feature-window bounds"};
    }
    double sum = 0.0;
    for (std::size_t index = begin; index < end; ++index) {
        sum += values[index];
    }
    return sum / static_cast<double>(end - begin);
}

[[nodiscard]] double window_stddev(
    const std::vector<double>& values,
    const std::size_t begin,
    const std::size_t end
) {
    const auto mean = window_mean(values, begin, end);
    double squared_error = 0.0;
    for (std::size_t index = begin; index < end; ++index) {
        const auto error = values[index] - mean;
        squared_error += error * error;
    }
    return std::sqrt(squared_error / static_cast<double>(end - begin));
}

[[nodiscard]] double realized_close_volatility(
    const PriceHistory& history,
    const std::size_t current_index,
    const std::size_t session_count
) {
    if (current_index < session_count) {
        throw std::invalid_argument{"Volatility features require sufficient trailing history"};
    }
    const auto first_return_index = current_index - session_count + 1;
    std::vector<double> log_returns;
    log_returns.reserve(session_count);
    for (std::size_t index = first_return_index; index <= current_index; ++index) {
        if (!(history.closes[index] > 0.0) || !(history.closes[index - 1] > 0.0)) {
            throw std::invalid_argument{"Market history contains a non-positive close"};
        }
        log_returns.push_back(std::log(history.closes[index] / history.closes[index - 1]));
    }
    return window_stddev(log_returns, 0, log_returns.size());
}

[[nodiscard]] arrakis::model::Dataset append_volatility_features(
    const arrakis::model::Dataset& source,
    const PriceHistory& market,
    const PriceHistory& benchmark
) {
    arrakis::model::Dataset result;
    result.feature_names = source.feature_names;
    result.feature_names.insert(
        result.feature_names.end(), kVolatilityFeatureNames.begin(), kVolatilityFeatureNames.end()
    );
    result.dates.reserve(source.row_count());
    result.labels.reserve(source.row_count());
    result.features.reserve(
        source.row_count() * (source.feature_count() + kVolatilityFeatureNames.size())
    );

    for (std::size_t row = 0; row < source.row_count(); ++row) {
        const auto market_it = market.index_by_date.find(source.dates[row]);
        const auto benchmark_it = benchmark.index_by_date.find(source.dates[row]);
        if (market_it == market.index_by_date.end() || benchmark_it == benchmark.index_by_date.end()) {
            throw std::invalid_argument{
                "Volatility features require matching XLK and benchmark dates: " +
                source.dates[row]
            };
        }
        const auto market_index = market_it->second;
        const auto benchmark_index = benchmark_it->second;
        if (market_index < 60 || benchmark_index < 20 ||
            market_index == 0 || benchmark_index == 0) {
            throw std::invalid_argument{
                "Volatility features require 60 trailing XLK sessions and 20 benchmark sessions"
            };
        }
        if (!(market.opens[market_index] > 0.0) || !(market.closes[market_index] > 0.0) ||
            !(market.closes[market_index - 1] > 0.0) || !(market.lows[market_index] >= 0.0) ||
            !(market.highs[market_index] >= market.lows[market_index]) ||
            !(market.volumes[market_index] >= 0.0)) {
            throw std::invalid_argument{"XLK history contains invalid OHLCV values"};
        }

        const auto overnight_gap =
            market.opens[market_index] / market.closes[market_index - 1] - 1.0;
        const auto intraday_range =
            (market.highs[market_index] - market.lows[market_index]) /
            market.closes[market_index];
        const auto open_to_close_abs =
            std::abs(market.closes[market_index] / market.opens[market_index] - 1.0);
        const auto volume_mean = window_mean(
            market.volumes, market_index - 20 + 1, market_index + 1
        );
        const auto volume_stddev = window_stddev(
            market.volumes, market_index - 20 + 1, market_index + 1
        );
        const auto volume_z = volume_stddev > 1.0e-12
                                  ? (market.volumes[market_index] - volume_mean) / volume_stddev
                                  : 0.0;
        const std::array<double, kVolatilityFeatureNames.size()> volatility_values{
            overnight_gap,
            intraday_range,
            open_to_close_abs,
            realized_close_volatility(market, market_index, 5),
            realized_close_volatility(market, market_index, 10),
            realized_close_volatility(market, market_index, 20),
            realized_close_volatility(market, market_index, 60),
            volume_z,
            realized_close_volatility(benchmark, benchmark_index, 20),
        };
        for (const auto value : volatility_values) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument{"Volatility feature calculation produced a non-finite value"};
            }
        }

        result.dates.push_back(source.dates[row]);
        result.labels.push_back(source.labels[row]);
        const auto source_begin = row * source.feature_count();
        result.features.insert(
            result.features.end(),
            source.features.begin() + static_cast<std::ptrdiff_t>(source_begin),
            source.features.begin() + static_cast<std::ptrdiff_t>(
                source_begin + source.feature_count()
            )
        );
        result.features.insert(result.features.end(), volatility_values.begin(), volatility_values.end());
    }

    return result;
}

[[nodiscard]] arrakis::model::Dataset append_context_volatility_features(
    const arrakis::model::Dataset& source,
    const PriceHistory& qqq,
    const PriceHistory& vix
) {
    arrakis::model::Dataset result;
    result.feature_names = source.feature_names;
    result.feature_names.insert(
        result.feature_names.end(),
        kContextVolatilityFeatureNames.begin(),
        kContextVolatilityFeatureNames.end()
    );
    result.dates.reserve(source.row_count());
    result.labels.reserve(source.row_count());
    result.features.reserve(
        source.row_count() * (source.feature_count() + kContextVolatilityFeatureNames.size())
    );

    for (std::size_t row = 0; row < source.row_count(); ++row) {
        const auto qqq_it = qqq.index_by_date.find(source.dates[row]);
        const auto vix_it = vix.index_by_date.find(source.dates[row]);
        if (qqq_it == qqq.index_by_date.end() || vix_it == vix.index_by_date.end()) {
            throw std::invalid_argument{
                "Context volatility features require matching QQQ and VIX dates: " +
                source.dates[row]
            };
        }
        const auto qqq_index = qqq_it->second;
        const auto vix_index = vix_it->second;
        if (qqq_index < 20 || vix_index == 0 || !(vix.closes[vix_index] > 0.0) ||
            !(vix.closes[vix_index - 1] > 0.0)) {
            throw std::invalid_argument{"Context volatility features require sufficient history"};
        }
        const std::array<double, kContextVolatilityFeatureNames.size()> values{
            realized_close_volatility(qqq, qqq_index, 20),
            vix.closes[vix_index],
            vix.closes[vix_index] / vix.closes[vix_index - 1] - 1.0,
        };
        for (const auto value : values) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument{"Context volatility feature is non-finite"};
            }
        }

        result.dates.push_back(source.dates[row]);
        result.labels.push_back(source.labels[row]);
        const auto source_begin = row * source.feature_count();
        result.features.insert(
            result.features.end(),
            source.features.begin() + static_cast<std::ptrdiff_t>(source_begin),
            source.features.begin() + static_cast<std::ptrdiff_t>(
                source_begin + source.feature_count()
            )
        );
        result.features.insert(result.features.end(), values.begin(), values.end());
    }
    return result;
}

[[nodiscard]] arrakis::model::Dataset add_requested_features(
    arrakis::model::Dataset dataset,
    const Options& options
) {
    if (!options.volatility_features && !options.context_volatility_features) return dataset;
    const auto market = load_price_history(options.market_data);
    const auto benchmark = load_price_history(options.benchmark_data);
    auto result = append_volatility_features(dataset, market, benchmark);
    if (!options.context_volatility_features) return result;
    const auto qqq = load_price_history(options.qqq_data);
    const auto vix = load_vix_history(options.vix_data);
    return append_context_volatility_features(result, qqq, vix);
}

[[nodiscard]] arrakis::model::Dataset select_feature_subset(
    const arrakis::model::Dataset& source,
    const std::string_view subset
) {
    const auto has_embedding_columns = [&]() {
        return std::ranges::any_of(source.feature_names, [](const auto& name) {
            return name.starts_with("embedding_");
        });
    };
    const auto select_names = [&](const auto& expected_names) {
        std::vector<std::size_t> indices;
        indices.reserve(expected_names.size());
        for (const auto expected : expected_names) {
            const auto found = std::ranges::find(source.feature_names, expected);
            if (found == source.feature_names.end()) {
                throw std::invalid_argument{
                    "Dataset is missing required feature: " + std::string{expected}
                };
            }
            indices.push_back(static_cast<std::size_t>(
                std::distance(source.feature_names.begin(), found)
            ));
        }

        for (const auto index : indices) {
            for (std::size_t row = 0; row < source.row_count(); ++row) {
                const auto value = source.features[row * source.feature_count() + index];
                if (!std::isfinite(value)) {
                    throw std::invalid_argument{
                        "Required feature contains a non-finite value: " +
                        source.feature_names[index]
                    };
                }
            }
        }

        arrakis::model::Dataset result;
        result.dates = source.dates;
        result.labels = source.labels;
        result.feature_names.reserve(indices.size());
        for (const auto index : indices) result.feature_names.push_back(source.feature_names[index]);
        result.features.reserve(result.row_count() * indices.size());
        for (std::size_t row = 0; row < source.row_count(); ++row) {
            for (const auto index : indices) {
                result.features.push_back(source.features[row * source.feature_count() + index]);
            }
        }
        return result;
    };

    if (subset == "market") {
        return select_names(arrakis::news::kMarketFeatureNames);
    }
    if (subset == "market-volatility") {
        return select_names(kMarketVolatilityFeatureNames);
    }
    if (subset == "market-context-volatility") {
        return select_names(kMarketContextVolatilityFeatureNames);
    }
    if (subset == "logits-only") {
        return select_names(arrakis::news::kLogitsOnlyNewsFeatureNames);
    }
    if (subset == "news") {
        if (has_embedding_columns()) return select_names(arrakis::news::kNewsFeatureNames);
        return select_names(arrakis::news::kLogitsOnlyNewsFeatureNames);
    }
    if (subset == "combined") {
        if (has_embedding_columns()) {
            bool embedding_has_signal = false;
            for (std::size_t index = 0; index < source.feature_count(); ++index) {
                if (!source.feature_names[index].starts_with("embedding_")) continue;
                for (std::size_t row = 0; row < source.row_count(); ++row) {
                    if (std::abs(source.features[row * source.feature_count() + index]) > 1e-12F) {
                        embedding_has_signal = true;
                        break;
                    }
                }
                if (embedding_has_signal) break;
            }
            if (!embedding_has_signal) {
                throw std::invalid_argument{
                    "Combined dataset contains zero-variance embedding columns; "
                    "use --feature-subset logits-only or provide a FinBERT graph "
                    "with a pooled embedding output"
                };
            }
        }
        for (const auto value : source.features) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument{
                    "Combined dataset contains a non-finite feature value"
                };
            }
        }
        return source;
    }
    throw std::invalid_argument{
        "--feature-subset must be combined, market, market-volatility, "
        "market-context-volatility, news, or logits-only"
    };
}

[[nodiscard]] Options parse_options(const int argc, char** argv) {
    Options options;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto require_value = [&]() -> std::string_view {
            if (index + 1 >= argc) {
                throw std::invalid_argument{"Missing value after " + std::string{argument}};
            }
            ++index;
            return argv[index];
        };

        if (argument == "--input") {
            options.input = require_value();
        } else if (argument == "--model-output") {
            options.model_output = require_value();
        } else if (argument == "--walk-forward") {
            options.walk_forward = true;
        } else if (argument == "--walk-forward-output") {
            options.walk_forward_output = require_value();
        } else if (argument == "--monthly-walk-forward") {
            options.monthly_walk_forward = true;
        } else if (argument == "--monthly-walk-forward-output") {
            options.monthly_walk_forward_output = require_value();
        } else if (argument == "--ablation") {
            options.ablation = true;
        } else if (argument == "--ablation-output") {
            options.ablation_output = require_value();
        } else if (argument == "--volatility-features") {
            options.volatility_features = true;
        } else if (argument == "--context-volatility-features") {
            options.volatility_features = true;
            options.context_volatility_features = true;
        } else if (argument == "--purge-sessions") {
            options.purge_sessions = std::stoi(std::string{require_value()});
        } else if (argument == "--test-month-start") {
            options.test_month_start = require_value();
        } else if (argument == "--test-month-end") {
            options.test_month_end = require_value();
        } else if (argument == "--train-window-months") {
            options.train_window_months = static_cast<std::size_t>(std::stoull(std::string{require_value()}));
        } else if (argument == "--target") {
            options.target = require_value();
        } else if (argument == "--feature-subset") {
            options.feature_subset = require_value();
        } else if (argument == "--market-data") {
            options.market_data = require_value();
        } else if (argument == "--benchmark-data") {
            options.benchmark_data = require_value();
        } else if (argument == "--qqq-data") {
            options.qqq_data = require_value();
        } else if (argument == "--vix-data") {
            options.vix_data = require_value();
        } else if (argument == "--validation-fraction") {
            options.validation_fraction = std::stod(std::string{require_value()});
        } else if (argument == "--rounds") {
            options.rounds = std::stoi(std::string{require_value()});
        } else if (argument == "--early-stopping-rounds") {
            options.early_stopping_rounds = std::stoi(std::string{require_value()});
        } else if (argument == "--eta") {
            options.hyperparameters.eta = std::stod(std::string{require_value()});
        } else if (argument == "--max-depth") {
            options.hyperparameters.max_depth = std::stoi(std::string{require_value()});
        } else if (argument == "--min-child-weight") {
            options.hyperparameters.min_child_weight = std::stoi(std::string{require_value()});
        } else if (argument == "--subsample") {
            options.hyperparameters.subsample = std::stod(std::string{require_value()});
        } else if (argument == "--colsample-bytree") {
            options.hyperparameters.colsample_bytree = std::stod(std::string{require_value()});
        } else if (argument == "--lambda") {
            options.hyperparameters.lambda = std::stod(std::string{require_value()});
        } else if (argument == "--alpha") {
            options.hyperparameters.alpha = std::stod(std::string{require_value()});
        } else if (argument == "--seed") {
            options.hyperparameters.seed = std::stoi(std::string{require_value()});
        } else if (argument == "--train-end") {
            options.train_end = require_value();
        } else if (argument == "--validation-end") {
            options.validation_end = require_value();
        } else if (argument == "--test-end") {
            options.test_end = require_value();
        } else if (argument == "--hyperparameter-search" || argument == "--search") {
            options.hyperparameter_search = true;
        } else if (argument == "--help") {
            std::cout
                << "Usage: arrakis-train-xgboost --input <features.csv> [options]\n\n"
                << "Options:\n"
                << "  --target <name>                 Target column or target_high_volatility_next_day,\n"
                << "                                  forward_return_3d_up, forward_return_5d_up,\n"
                << "                                  forward_return_5d_open_to_close_up,\n"
                << "                                  excess_return_3d_up, excess_return_5d_up\n"
                << "  --feature-subset <name>         combined, market, market-volatility, "
                   "market-context-volatility, news, or logits-only\n"
                << "  --market-data <path>             XLK history for alternative targets\n"
                << "  --benchmark-data <path>          SPY history for excess-return targets\n"
                << "  --volatility-features            Append leakage-safe OHLCV volatility features\n"
                << "  --context-volatility-features    Also append QQQ realized volatility and VIX\n"
                << "  --qqq-data <path>                QQQ history for context volatility features\n"
                << "  --vix-data <path>                VIX history for context volatility features\n"
                << "  --model-output <path>            XGBoost model output path\n"
                << "  --walk-forward                  Evaluate all six targets on expanding windows\n"
                << "  --walk-forward-output <path>    Walk-forward JSON output path\n"
                << "  --monthly-walk-forward          Purged monthly next-close evaluation\n"
                << "  --monthly-walk-forward-output <path>  Monthly evaluation JSON path\n"
                << "  --purge-sessions <count>        Sessions removed before validation/test (default: 1)\n"
                << "  --test-month-start <YYYY-MM>   Restrict outer test months from this month\n"
                << "  --test-month-end <YYYY-MM>     Restrict outer test months through this month\n"
                << "  --train-window-months <n>      Use a rolling n-month training window; 0 is expanding\n"
                << "  --ablation                      Prior/market/logits/combined comparison\n"
                << "  --ablation-output <path>        Ablation JSON path\n"
                << "  --validation-fraction <0-0.5>    Most recent fraction for validation\n"
                << "  --train-end <YYYY-MM-DD>         Exact training boundary\n"
                << "  --validation-end <YYYY-MM-DD>    Exact validation boundary\n"
                << "  --test-end <YYYY-MM-DD>          Exact held-out boundary\n"
                << "  --rounds <count>                 Boosting rounds (default: 75)\n"
                << "  --early-stopping-rounds <count>  Validation rounds without improvement (default: 20)\n"
                << "  --eta <value> --max-depth <n>    XGBoost hyperparameters\n"
                << "  --min-child-weight <n>           XGBoost hyperparameter\n"
                << "  --subsample <0-1> --colsample-bytree <0-1>\n"
                << "  --lambda <value> --alpha <value> Regularization hyperparameters\n"
                << "  --seed <integer>                Reproducible XGBoost seed (default: 42)\n"
                << "  --hyperparameter-search          Validation-only grid search\n";
            std::exit(0);
        } else {
            throw std::invalid_argument{"Unknown argument: " + std::string{argument}};
        }
    }

    if (options.input.empty()) {
        throw std::invalid_argument{"--input is required"};
    }
    if (!(options.validation_fraction > 0.0 && options.validation_fraction < 0.5)) {
        throw std::invalid_argument{"--validation-fraction must be greater than 0 and less than 0.5"};
    }
    if (options.rounds <= 0) {
        throw std::invalid_argument{"--rounds must be positive"};
    }
    if (options.early_stopping_rounds < 0) {
        throw std::invalid_argument{"--early-stopping-rounds cannot be negative"};
    }
    if (options.purge_sessions < 0) {
        throw std::invalid_argument{"--purge-sessions cannot be negative"};
    }
    if (options.hyperparameters.seed < 0) {
        throw std::invalid_argument{"--seed cannot be negative"};
    }
    const auto valid_month = [](const std::string& value) {
        return value.empty() ||
               (value.size() == 7 && value[4] == '-' && value.substr(0, 4).find_first_not_of("0123456789") == std::string::npos &&
                value.substr(5, 2).find_first_not_of("0123456789") == std::string::npos);
    };
    if (!valid_month(options.test_month_start) || !valid_month(options.test_month_end) ||
        (!options.test_month_start.empty() && !options.test_month_end.empty() &&
         options.test_month_start > options.test_month_end)) {
        throw std::invalid_argument{"Test month boundaries must be ordered YYYY-MM values"};
    }
    if ((options.walk_forward || options.monthly_walk_forward || options.ablation) &&
        options.hyperparameter_search) {
        throw std::invalid_argument{
            "--hyperparameter-search is not permitted with walk-forward or ablation evaluation"
        };
    }
    if (options.walk_forward &&
        (!options.train_end.empty() || !options.validation_end.empty() ||
         !options.test_end.empty())) {
        throw std::invalid_argument{
            "Exact split boundaries are not permitted with --walk-forward"
        };
    }
    if ((options.monthly_walk_forward || options.ablation) &&
        (!options.train_end.empty() || !options.validation_end.empty() ||
         !options.test_end.empty())) {
        throw std::invalid_argument{
            "Exact split boundaries are not permitted with monthly or ablation evaluation"
        };
    }
    const auto& parameters = options.hyperparameters;
    if (!(parameters.eta > 0.0) || parameters.max_depth <= 0 ||
        parameters.min_child_weight < 0 || parameters.lambda < 0.0 || parameters.alpha < 0.0 ||
        !(parameters.subsample > 0.0 && parameters.subsample <= 1.0) ||
        !(parameters.colsample_bytree > 0.0 && parameters.colsample_bytree <= 1.0)) {
        throw std::invalid_argument{"Invalid XGBoost hyperparameter value"};
    }

    const auto exact_split = !options.train_end.empty() || !options.validation_end.empty() ||
                             !options.test_end.empty();
    if (exact_split && (options.train_end.empty() || options.validation_end.empty() ||
                        options.test_end.empty())) {
        throw std::invalid_argument{"All exact split boundaries are required together"};
    }
    if (!alternative_target_spec(options.target).has_value() &&
        options.target != "target_next_close_up") {
        throw std::invalid_argument{"Unsupported target: " + options.target};
    }
    if (alternative_target_spec(options.target).has_value() && options.market_data.empty()) {
        throw std::invalid_argument{"Alternative targets require --market-data"};
    }
    if (alternative_target_spec(options.target).has_value() &&
        alternative_target_spec(options.target)->excess_return && options.benchmark_data.empty()) {
        throw std::invalid_argument{"Excess-return targets require --benchmark-data"};
    }
    if (options.volatility_features &&
        (options.market_data.empty() || options.benchmark_data.empty())) {
        throw std::invalid_argument{
            "--volatility-features requires --market-data and --benchmark-data"
        };
    }
    if (options.context_volatility_features &&
        (options.qqq_data.empty() || options.vix_data.empty())) {
        throw std::invalid_argument{
            "--context-volatility-features requires --qqq-data and --vix-data"
        };
    }
    return options;
}

[[nodiscard]] double parse_evaluation_metric(
    const std::string& evaluation,
    const std::string_view metric_name
) {
    const auto marker = std::string{metric_name} + ":";
    const auto marker_position = evaluation.find(marker);
    if (marker_position == std::string::npos) {
        throw std::runtime_error{"XGBoost evaluation did not contain " + marker};
    }
    const auto value_begin = marker_position + marker.size();
    const auto value_end = evaluation.find_first_of("\t ", value_begin);
    return std::stod(evaluation.substr(value_begin, value_end - value_begin));
}

void set_hyperparameters(Booster& booster, const Hyperparameters& parameters) {
    booster.set_parameter("objective", "binary:logistic");
    booster.set_parameter("eval_metric", "logloss");
    booster.set_parameter("tree_method", "hist");
    booster.set_parameter("device", "cpu");
    booster.set_parameter("eta", format_double(parameters.eta));
    booster.set_parameter("max_depth", std::to_string(parameters.max_depth));
    booster.set_parameter("min_child_weight", std::to_string(parameters.min_child_weight));
    booster.set_parameter("subsample", format_double(parameters.subsample));
    booster.set_parameter("colsample_bytree", format_double(parameters.colsample_bytree));
    booster.set_parameter("lambda", format_double(parameters.lambda));
    booster.set_parameter("alpha", format_double(parameters.alpha));
    booster.set_parameter("seed", std::to_string(parameters.seed));
}

[[nodiscard]] TrainingResult train_booster(
    const DMatrixHandle train,
    const DMatrixHandle validation,
    const Hyperparameters& parameters,
    const int rounds,
    const int early_stopping_rounds,
    const std::filesystem::path& checkpoint,
    const bool log_progress
) {
    const std::vector<DMatrixHandle> matrices{train, validation};
    const std::vector<const char*> matrix_names{"train", "validation"};
    Booster booster{matrices};
    set_hyperparameters(booster, parameters);

    if (!checkpoint.empty()) {
        std::error_code remove_error;
        std::filesystem::remove(checkpoint, remove_error);
    }

    TrainingResult result;
    int rounds_without_improvement = 0;
    for (int iteration = 0; iteration < rounds; ++iteration) {
        booster.update(iteration, train);
        const auto evaluation = booster.evaluate(iteration, matrices, matrix_names);
        const auto validation_logloss = parse_evaluation_metric(evaluation, "validation-logloss");
        result.rounds_run = iteration + 1;

        if (validation_logloss < result.best_validation_logloss) {
            result.best_validation_logloss = validation_logloss;
            result.best_iteration_index = iteration;
            rounds_without_improvement = 0;
            if (!checkpoint.empty()) {
                booster.save(checkpoint);
            }
        } else {
            ++rounds_without_improvement;
        }

        if (log_progress &&
            (iteration == 0 || (iteration + 1) % 10 == 0 ||
             rounds_without_improvement == 0 ||
             iteration + 1 == rounds)) {
            std::cout << evaluation << '\n';
        }
        if (early_stopping_rounds > 0 && rounds_without_improvement >= early_stopping_rounds) {
            break;
        }
    }

    if (result.best_iteration_index < 0) {
        throw std::runtime_error{"Training did not produce a validation score"};
    }
    return result;
}

[[nodiscard]] std::vector<SearchResult> run_hyperparameter_search(
    const DMatrixHandle train,
    const DMatrixHandle validation,
    const Options& options,
    Hyperparameters& selected
) {
    const std::array<double, 3> etas{0.01, 0.05, 0.1};
    const std::array<std::pair<double, double>, 3> regularization{
        std::pair<double, double>{1.0, 0.0},
        std::pair<double, double>{10.0, 1.0},
        std::pair<double, double>{25.0, 5.0},
    };
    const std::array<std::pair<double, double>, 2> sampling{
        std::pair<double, double>{0.8, 0.8},
        std::pair<double, double>{1.0, 1.0},
    };

    std::vector<SearchResult> results;
    for (int max_depth = 2; max_depth <= 5; ++max_depth) {
        for (const auto eta : etas) {
            for (const auto [lambda, alpha] : regularization) {
                for (const auto [subsample, colsample] : sampling) {
                    Hyperparameters candidate{
                        .eta = eta,
                        .max_depth = max_depth,
                        .min_child_weight = options.hyperparameters.min_child_weight,
                        .subsample = subsample,
                        .colsample_bytree = colsample,
                        .lambda = lambda,
                        .alpha = alpha,
                        .seed = options.hyperparameters.seed,
                    };
                    const auto training = train_booster(
                        train,
                        validation,
                        candidate,
                        options.rounds,
                        options.early_stopping_rounds,
                        {},
                        false
                    );
                    results.push_back(SearchResult{
                        .hyperparameters = candidate,
                        .validation_logloss = training.best_validation_logloss,
                        .best_iteration = training.best_iteration_index + 1,
                    });
                    if (results.size() == 1 ||
                        training.best_validation_logloss < results.front().validation_logloss) {
                        selected = candidate;
                        std::swap(results.front(), results.back());
                    }
                }
            }
        }
    }

    std::ranges::sort(results, {}, &SearchResult::validation_logloss);
    return results;
}

[[nodiscard]] std::vector<YearMetrics> evaluate_by_year(
    const arrakis::model::Dataset& dataset,
    const std::vector<float>& probabilities
) {
    struct Samples final {
        std::vector<float> labels;
        std::vector<float> probabilities;
    };
    std::map<int, Samples> grouped;
    for (std::size_t index = 0; index < dataset.row_count(); ++index) {
        const auto year = std::stoi(dataset.dates[index].substr(0, 4));
        grouped[year].labels.push_back(dataset.labels[index]);
        grouped[year].probabilities.push_back(probabilities[index]);
    }

    std::vector<YearMetrics> result;
    for (auto& [year, samples] : grouped) {
        std::size_t positives = 0;
        for (const auto label : samples.labels) {
            positives += static_cast<std::size_t>(label == 1.0F);
        }
        std::optional<arrakis::model::BinaryMetrics> metrics;
        if (positives > 0 && positives < samples.labels.size()) {
            metrics = arrakis::model::evaluate_binary_classifier(
                samples.labels, samples.probabilities
            );
        }
        result.push_back(YearMetrics{
            .year = year,
            .rows = samples.labels.size(),
            .metrics = metrics,
        });
    }
    return result;
}

void write_metric_object(
    std::ostream& output,
    const arrakis::model::BinaryMetrics& metrics,
    const std::string_view indent
) {
    output << indent << "\"accuracy\": " << metrics.accuracy << ",\n"
           << indent << "\"log_loss\": " << metrics.log_loss << ",\n"
           << indent << "\"roc_auc\": " << metrics.roc_auc << ",\n"
           << indent << "\"positive_rate\": " << metrics.positive_rate << ",\n"
           << indent << "\"mean_probability\": " << metrics.mean_probability << '\n';
}

[[nodiscard]] std::string year_start(const int year) {
    return std::to_string(year) + "-01-01";
}

[[nodiscard]] std::string year_end(const int year) {
    return std::to_string(year) + "-12-31";
}

[[nodiscard]] std::optional<arrakis::model::BinaryMetrics> evaluate_if_binary(
    const arrakis::model::Dataset& dataset,
    const std::vector<float>& probabilities
) {
    if (dataset.labels.empty() || dataset.labels.size() != probabilities.size()) {
        throw std::invalid_argument{"Walk-forward labels and predictions are misaligned"};
    }
    const auto positive_count = static_cast<std::size_t>(
        std::ranges::count(dataset.labels, 1.0F)
    );
    if (positive_count == 0 || positive_count == dataset.labels.size()) {
        return std::nullopt;
    }
    return arrakis::model::evaluate_binary_classifier(dataset.labels, probabilities);
}

[[nodiscard]] std::optional<arrakis::model::BinaryMetrics> evaluate_if_binary(
    const std::vector<float>& labels,
    const std::vector<float>& probabilities
) {
    if (labels.empty() || labels.size() != probabilities.size()) {
        throw std::invalid_argument{"Labels and predictions are misaligned"};
    }
    const auto positive_count = static_cast<std::size_t>(std::ranges::count(labels, 1.0F));
    if (positive_count == 0 || positive_count == labels.size()) return std::nullopt;
    return arrakis::model::evaluate_binary_classifier(labels, probabilities);
}

struct PlattCalibrator final {
    double intercept{};
    double slope{1.0};

    [[nodiscard]] double apply(const double probability) const {
        const auto bounded = std::clamp(probability, 1.0e-6, 1.0 - 1.0e-6);
        const auto logit = std::log(bounded / (1.0 - bounded));
        const auto linear = std::clamp(intercept + slope * logit, -30.0, 30.0);
        return 1.0 / (1.0 + std::exp(-linear));
    }
};

[[nodiscard]] PlattCalibrator fit_platt_calibrator(
    const std::vector<float>& labels,
    const std::vector<float>& probabilities
) {
    if (labels.empty() || labels.size() != probabilities.size()) {
        throw std::invalid_argument{"Platt calibration inputs are misaligned"};
    }

    const auto positive_count = static_cast<double>(std::ranges::count(labels, 1.0F));
    const auto sample_count = static_cast<double>(labels.size());
    const auto smoothed_prior = (positive_count + 0.5) / (sample_count + 1.0);
    PlattCalibrator calibrator{
        .intercept = std::log(smoothed_prior / (1.0 - smoothed_prior)),
        .slope = 1.0,
    };
    if (positive_count == 0.0 || positive_count == sample_count) {
        calibrator.slope = 0.0;
        return calibrator;
    }

    for (int iteration = 0; iteration < 25; ++iteration) {
        double gradient_intercept = 0.0;
        double gradient_slope = 0.0;
        double hessian_intercept_intercept = 1.0e-6;
        double hessian_intercept_slope = 0.0;
        double hessian_slope_slope = 1.0e-6;
        for (std::size_t index = 0; index < labels.size(); ++index) {
            const auto bounded = std::clamp(
                static_cast<double>(probabilities[index]), 1.0e-6, 1.0 - 1.0e-6
            );
            const auto x = std::log(bounded / (1.0 - bounded));
            const auto linear = std::clamp(
                calibrator.intercept + calibrator.slope * x, -30.0, 30.0
            );
            const auto fitted = 1.0 / (1.0 + std::exp(-linear));
            const auto residual = fitted - static_cast<double>(labels[index]);
            const auto curvature = std::max(fitted * (1.0 - fitted), 1.0e-8);
            gradient_intercept += residual;
            gradient_slope += residual * x;
            hessian_intercept_intercept += curvature;
            hessian_intercept_slope += curvature * x;
            hessian_slope_slope += curvature * x * x;
        }

        const auto determinant = hessian_intercept_intercept * hessian_slope_slope -
                                hessian_intercept_slope * hessian_intercept_slope;
        if (!(determinant > 1.0e-12)) break;
        const auto delta_intercept =
            (hessian_slope_slope * gradient_intercept -
             hessian_intercept_slope * gradient_slope) /
            determinant;
        const auto delta_slope =
            (-hessian_intercept_slope * gradient_intercept +
             hessian_intercept_intercept * gradient_slope) /
            determinant;
        calibrator.intercept = std::clamp(calibrator.intercept - delta_intercept, -20.0, 20.0);
        calibrator.slope = std::clamp(calibrator.slope - delta_slope, -20.0, 20.0);
        if (std::abs(delta_intercept) < 1.0e-7 && std::abs(delta_slope) < 1.0e-7) break;
    }
    // Keep the fold-local calibration conservative on small monthly validation
    // windows. The bounds are fixed before evaluation and prevent a handful of
    // validation observations from turning near-0.5 probabilities into extremes.
    calibrator.intercept = std::clamp(calibrator.intercept, -2.0, 2.0);
    calibrator.slope = std::clamp(calibrator.slope, 0.75, 1.25);
    return calibrator;
}

[[nodiscard]] std::vector<float> apply_platt_calibrator(
    const PlattCalibrator& calibrator,
    const std::vector<float>& probabilities
) {
    std::vector<float> calibrated;
    calibrated.reserve(probabilities.size());
    for (const auto probability : probabilities) {
        calibrated.push_back(static_cast<float>(calibrator.apply(probability)));
    }
    return calibrated;
}

void write_optional_metric_object(
    std::ostream& output,
    const std::optional<arrakis::model::BinaryMetrics>& metrics,
    const std::string_view indent
) {
    if (!metrics.has_value()) {
        output << "null";
        return;
    }
    output << "{\n";
    write_metric_object(output, *metrics, indent);
    output << indent.substr(0, indent.size() >= 2 ? indent.size() - 2 : 0) << "}";
}

struct MonthRange final {
    std::string key;
    std::size_t begin{};
    std::size_t end{};
};

[[nodiscard]] std::string session_key(const std::string_view date) {
    const auto separator = date.find('|');
    return std::string{date.substr(0, separator)};
}

[[nodiscard]] std::size_t row_before_purged_sessions(
    const arrakis::model::Dataset& dataset,
    std::size_t boundary,
    const std::size_t sessions
) {
    std::size_t purged = 0;
    while (boundary > 0U && purged < sessions) {
        const auto last_session = session_key(dataset.dates[boundary - 1U]);
        do {
            --boundary;
        } while (boundary > 0U && session_key(dataset.dates[boundary - 1U]) == last_session);
        ++purged;
    }
    return boundary;
}

[[nodiscard]] std::vector<MonthRange> build_month_ranges(
    const arrakis::model::Dataset& dataset
) {
    std::vector<MonthRange> ranges;
    for (std::size_t index = 0; index < dataset.row_count(); ++index) {
        const auto key = dataset.dates[index].substr(0, 7);
        if (ranges.empty() || ranges.back().key != key) {
            ranges.push_back(MonthRange{.key = key, .begin = index, .end = index + 1});
        } else {
            ranges.back().end = index + 1;
        }
    }
    return ranges;
}

[[nodiscard]] MonthlyEvaluationResult run_monthly_evaluation(
    const arrakis::model::Dataset& dataset,
    const Options& options,
    const bool prior_baseline
) {
    if (dataset.row_count() < 10 || dataset.feature_count() == 0) {
        throw std::invalid_argument{"Monthly evaluation requires a non-empty feature dataset"};
    }
    const auto ranges = build_month_ranges(dataset);
    if (ranges.size() < 12) {
        throw std::invalid_argument{"Monthly evaluation requires at least 12 calendar months"};
    }

    MonthlyEvaluationResult result{.feature_subset =
                                       prior_baseline ? "class-prior" : options.feature_subset};
    const auto first_year = std::stoi(ranges.front().key.substr(0, 4));
    for (std::size_t test_month_index = 0; test_month_index < ranges.size(); ++test_month_index) {
        const auto& test_range = ranges[test_month_index];
        if (std::stoi(test_range.key.substr(0, 4)) < first_year + 2 || test_month_index < 6 ||
            (!options.test_month_start.empty() && test_range.key < options.test_month_start) ||
            (!options.test_month_end.empty() && test_range.key > options.test_month_end)) {
            continue;
        }

        // The validation range starts six calendar months before the test
        // month and extends to the purged test boundary, giving the trainer
        // the full trailing six-month validation window recorded in the
        // evaluation protocol.
        const auto validation_start = ranges[test_month_index - 6].begin;
        const auto purge_count = static_cast<std::size_t>(options.purge_sessions);
        const auto validation_end = row_before_purged_sessions(
            dataset, test_range.begin, purge_count
        );
        const auto train_end = row_before_purged_sessions(dataset, validation_start, purge_count);
        if (validation_end <= validation_start || train_end == 0U) {
            continue;
        }
        const auto validation_month_index = test_month_index - 6;
        if (options.train_window_months > validation_month_index) continue;
        const auto train_start = options.train_window_months == 0
            ? std::size_t{0}
            : ranges[validation_month_index - options.train_window_months].begin;
        if (train_end <= train_start || validation_end <= validation_start ||
            test_range.begin >= test_range.end) {
            continue;
        }

        const auto train = arrakis::model::row_slice(dataset, train_start, train_end);
        const auto validation = arrakis::model::row_slice(
            dataset, validation_start, validation_end
        );
        const auto test = arrakis::model::row_slice(dataset, test_range.begin, test_range.end);

        std::vector<float> validation_predictions;
        std::vector<float> test_predictions;
        std::vector<float> calibrated_test_predictions;
        int selected_best_iteration = 0;
        if (prior_baseline) {
            const auto positive_count = static_cast<double>(
                std::ranges::count(train.labels, 1.0F)
            );
            const auto prior = static_cast<float>(
                positive_count / static_cast<double>(train.labels.size())
            );
            validation_predictions.assign(validation.row_count(), prior);
            test_predictions.assign(test.row_count(), prior);
            calibrated_test_predictions = test_predictions;
        } else {
            const DMatrix train_matrix{train};
            const DMatrix validation_matrix{validation};
            const DMatrix test_matrix{test};
            auto checkpoint = options.monthly_walk_forward_output;
            checkpoint += "." + result.feature_subset + "." + test_range.key + ".best.tmp.ubj";
            const auto training = train_booster(
                train_matrix.get(),
                validation_matrix.get(),
                options.hyperparameters,
                options.rounds,
                options.early_stopping_rounds,
                checkpoint,
                false
            );
            const std::vector<DMatrixHandle> matrices{
                train_matrix.get(), validation_matrix.get()
            };
            Booster booster{matrices};
            booster.load(checkpoint);
            std::error_code checkpoint_error;
            std::filesystem::remove(checkpoint, checkpoint_error);
            validation_predictions = booster.predict(validation_matrix.get());
            test_predictions = booster.predict(test_matrix.get());
            calibrated_test_predictions = apply_platt_calibrator(
                fit_platt_calibrator(validation.labels, validation_predictions),
                test_predictions
            );
            selected_best_iteration = training.best_iteration_index + 1;
        }

        result.windows.push_back(MonthlyWindowResult{
            .test_month = test_range.key,
            .train_rows = train.row_count(),
            .validation_rows = validation.row_count(),
            .test_rows = test.row_count(),
            .train_start = train.dates.front(),
            .train_end = train.dates.back(),
            .validation_start = validation.dates.front(),
            .validation_end = validation.dates.back(),
            .test_start = test.dates.front(),
            .test_end = test.dates.back(),
            .selected_best_iteration = selected_best_iteration,
            .validation_metrics = evaluate_if_binary(validation, validation_predictions),
            .test_metrics = evaluate_if_binary(test, test_predictions),
            .calibrated_test_metrics = evaluate_if_binary(test, calibrated_test_predictions),
        });
        result.prediction_dates.insert(
            result.prediction_dates.end(), test.dates.begin(), test.dates.end()
        );
        result.prediction_labels.insert(
            result.prediction_labels.end(), test.labels.begin(), test.labels.end()
        );
        result.prediction_probabilities.insert(
            result.prediction_probabilities.end(), test_predictions.begin(), test_predictions.end()
        );
        result.calibrated_prediction_probabilities.insert(
            result.calibrated_prediction_probabilities.end(),
            calibrated_test_predictions.begin(),
            calibrated_test_predictions.end()
        );
    }

    if (result.windows.empty() || result.prediction_labels.empty()) {
        throw std::runtime_error{"Monthly evaluation produced no eligible folds"};
    }
    return result;
}

void write_monthly_evaluation(
    const std::filesystem::path& output_path,
    const std::filesystem::path& dataset_path,
    const Options& options,
    const MonthlyEvaluationResult& result
) {
    if (output_path.has_parent_path()) std::filesystem::create_directories(output_path.parent_path());
    auto predictions_path = output_path;
    predictions_path += ".oos_predictions.csv";
    std::ofstream predictions{predictions_path};
    if (!predictions) throw std::runtime_error{"Could not write OOS predictions"};
    predictions << "feature_subset,date,label,probability_positive_label,"
                   "calibrated_probability_positive_label\n";
    for (std::size_t index = 0; index < result.prediction_dates.size(); ++index) {
        predictions << result.feature_subset << ',' << result.prediction_dates[index] << ','
                    << result.prediction_labels[index] << ','
                    << result.prediction_probabilities[index] << ','
                    << result.calibrated_prediction_probabilities[index] << '\n';
    }

    std::ofstream output{output_path};
    if (!output) throw std::runtime_error{"Could not write monthly evaluation"};
    output << std::fixed << std::setprecision(6)
           << "{\n"
           << "  \"protocol\": {\n"
           << "    \"dataset_path\": \"" << dataset_path.string() << "\",\n"
           << "    \"feature_subset\": \"" << result.feature_subset << "\",\n"
           << "    \"target\": \"" << options.target << "\",\n"
           << "    \"outer_test_policy\": \"successive calendar-month windows from year two\",\n"
           << "    \"validation_policy\": \"trailing six calendar months\",\n"
           << "    \"purge_sessions\": " << options.purge_sessions << ",\n"
           << "    \"purge_unit\": \"distinct session key before the optional | sector suffix\",\n"
           << "    \"test_month_start\": \"" << options.test_month_start << "\",\n"
           << "    \"test_month_end\": \"" << options.test_month_end << "\",\n"
           << "    \"train_window_months\": " << options.train_window_months << ",\n"
           << "    \"calibration\": \"fold-local Platt fit on the trailing validation window;"
              " outer test remains untouched\",\n"
           << "    \"execution_policy\": \"evaluation predictions are saved; strategy execution is next-session open\"\n"
           << "  },\n"
           << "  \"windows\": [\n";
    for (std::size_t index = 0; index < result.windows.size(); ++index) {
        const auto& window = result.windows[index];
        output << "    {\n"
               << "      \"test_month\": \"" << window.test_month << "\",\n"
               << "      \"train_start\": \"" << window.train_start << "\",\n"
               << "      \"train_end\": \"" << window.train_end << "\",\n"
               << "      \"validation_start\": \"" << window.validation_start << "\",\n"
               << "      \"validation_end\": \"" << window.validation_end << "\",\n"
               << "      \"test_start\": \"" << window.test_start << "\",\n"
               << "      \"test_end\": \"" << window.test_end << "\",\n"
               << "      \"train_rows\": " << window.train_rows << ",\n"
               << "      \"validation_rows\": " << window.validation_rows << ",\n"
               << "      \"test_rows\": " << window.test_rows << ",\n"
               << "      \"purged_before_validation_and_test\": " << options.purge_sessions << ",\n"
               << "      \"selected_best_iteration\": " << window.selected_best_iteration << ",\n"
               << "      \"validation\": ";
        write_optional_metric_object(output, window.validation_metrics, "        ");
        output << ",\n      \"test\": ";
        write_optional_metric_object(output, window.test_metrics, "        ");
        output << ",\n      \"calibrated_test\": ";
        write_optional_metric_object(output, window.calibrated_test_metrics, "        ");
        output << "\n    }" << (index + 1 == result.windows.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"pooled_test\": ";
    write_optional_metric_object(
        output,
        evaluate_if_binary(result.prediction_labels, result.prediction_probabilities),
        "  "
    );
    output << ",\n  \"pooled_calibrated_test\": ";
    write_optional_metric_object(
        output,
        evaluate_if_binary(
            result.prediction_labels, result.calibrated_prediction_probabilities
        ),
        "  "
    );
    output << "\n}\n";
}

void print_monthly_result(const MonthlyEvaluationResult& result) {
    const auto metrics = evaluate_if_binary(
        result.prediction_labels, result.prediction_probabilities
    );
    std::cout << "Monthly walk-forward " << result.feature_subset << ": "
              << result.windows.size() << " folds";
    if (metrics.has_value()) {
        std::cout << ", pooled accuracy=" << metrics->accuracy
                  << ", logloss=" << metrics->log_loss << ", AUC=" << metrics->roc_auc;
    }
    const auto calibrated_metrics = evaluate_if_binary(
        result.prediction_labels, result.calibrated_prediction_probabilities
    );
    if (calibrated_metrics.has_value()) {
        std::cout << ", calibrated logloss=" << calibrated_metrics->log_loss;
    }
    std::cout << '\n';
}

[[nodiscard]] std::vector<WalkForwardTargetResult> run_walk_forward_targets(
    const arrakis::model::Dataset& base_dataset,
    const Options& options,
    const std::optional<PriceHistory>& market,
    const std::optional<PriceHistory>& benchmark
) {
    std::vector<WalkForwardTargetResult> results;
    results.reserve(kWalkForwardTargets.size());

    for (const auto target : kWalkForwardTargets) {
        auto dataset = arrakis::model::Dataset{};
        if (target == "target_next_close_up") {
            dataset = base_dataset;
        } else {
            const auto spec = alternative_target_spec(target);
            if (!spec.has_value() || !market.has_value()) {
                throw std::runtime_error{"Walk-forward target prerequisites are missing"};
            }
            dataset = apply_alternative_target(base_dataset, *spec, *market, benchmark);
        }
        dataset = add_requested_features(std::move(dataset), options);
        dataset = select_feature_subset(dataset, options.feature_subset);

        const auto first_year = std::stoi(dataset.dates.front().substr(0, 4));
        const auto last_year = std::stoi(dataset.dates.back().substr(0, 4));
        if (last_year - first_year < 2) {
            throw std::runtime_error{
                "Walk-forward evaluation requires at least three calendar years of data"
            };
        }

        WalkForwardTargetResult target_result{.target = std::string{target}};
        for (int test_year = first_year + 2; test_year <= last_year; ++test_year) {
            const auto train = arrakis::model::date_slice(
                dataset, dataset.dates.front(), year_end(test_year - 2)
            );
            const auto validation = arrakis::model::date_slice(
                dataset, year_start(test_year - 1), year_end(test_year - 1)
            );
            const auto test = arrakis::model::date_slice(
                dataset, year_start(test_year), year_end(test_year)
            );

            const DMatrix train_matrix{train};
            const DMatrix validation_matrix{validation};
            const DMatrix test_matrix{test};
            const auto checkpoint = std::filesystem::path{
                options.walk_forward_output.string() + "." + std::string{target} + "." +
                std::to_string(test_year) + ".best.tmp.ubj"
            };
            const auto training = train_booster(
                train_matrix.get(),
                validation_matrix.get(),
                options.hyperparameters,
                options.rounds,
                options.early_stopping_rounds,
                checkpoint,
                false
            );
            const std::vector<DMatrixHandle> matrices{
                train_matrix.get(), validation_matrix.get()
            };
            Booster booster{matrices};
            booster.load(checkpoint);
            std::error_code checkpoint_error;
            std::filesystem::remove(checkpoint, checkpoint_error);

            const auto validation_predictions = booster.predict(validation_matrix.get());
            const auto test_predictions = booster.predict(test_matrix.get());
            target_result.windows.push_back(WalkForwardWindowResult{
                .test_year = test_year,
                .train_rows = train.row_count(),
                .validation_rows = validation.row_count(),
                .test_rows = test.row_count(),
                .train_start = train.dates.front(),
                .train_end = train.dates.back(),
                .validation_start = validation.dates.front(),
                .validation_end = validation.dates.back(),
                .test_start = test.dates.front(),
                .test_end = test.dates.back(),
                .selected_best_iteration = training.best_iteration_index + 1,
                .validation_metrics = evaluate_if_binary(validation, validation_predictions),
                .test_metrics = evaluate_if_binary(test, test_predictions),
            });
        }

        const auto all_windows_clear = std::ranges::all_of(
            target_result.windows,
            [](const WalkForwardWindowResult& window) {
                return window.validation_metrics.has_value() && window.test_metrics.has_value() &&
                       window.validation_metrics->roc_auc > 0.55 &&
                       window.test_metrics->roc_auc > 0.55;
            }
        );
        target_result.promotion_candidate = target_result.windows.size() >= 2 && all_windows_clear;
        results.push_back(std::move(target_result));
    }
    return results;
}

void write_walk_forward_results(
    const std::filesystem::path& output_path,
    const std::filesystem::path& dataset_path,
    const std::string& feature_subset,
    const std::vector<WalkForwardTargetResult>& results
) {
    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream output{output_path};
    if (!output) {
        throw std::runtime_error{"Could not write walk-forward results: " + output_path.string()};
    }

    output << std::fixed << std::setprecision(6)
           << "{\n"
           << "  \"protocol\": {\n"
           << "    \"dataset_path\": \"" << dataset_path.string() << "\",\n"
           << "    \"feature_subset\": \"" << feature_subset << "\",\n"
           << "    \"train_policy\": \"expanding from the first available row through the year before validation\",\n"
           << "    \"validation_policy\": \"one calendar year immediately before the test year\",\n"
           << "    \"test_policy\": \"successive non-overlapping one-calendar-year windows\",\n"
           << "    \"selection_metric\": \"validation log loss with early stopping\",\n"
           << "    \"hyperparameter_search\": false,\n"
           << "    \"promotion_bar\": {\"validation_auc_gt\": 0.55, \"test_auc_gt\": 0.55, \"requires_all_windows\": true}\n"
           << "  },\n"
           << "  \"targets\": [\n";
    for (std::size_t target_index = 0; target_index < results.size(); ++target_index) {
        const auto& target = results[target_index];
        output << "    {\n"
               << "      \"target\": \"" << target.target << "\",\n"
               << "      \"promotion_candidate\": "
               << (target.promotion_candidate ? "true" : "false") << ",\n"
               << "      \"windows\": [\n";
        for (std::size_t window_index = 0; window_index < target.windows.size(); ++window_index) {
            const auto& window = target.windows[window_index];
            output << "        {\n"
                   << "          \"test_year\": " << window.test_year << ",\n"
                   << "          \"train_start\": \"" << window.train_start << "\",\n"
                   << "          \"train_end\": \"" << window.train_end << "\",\n"
                   << "          \"validation_start\": \"" << window.validation_start << "\",\n"
                   << "          \"validation_end\": \"" << window.validation_end << "\",\n"
                   << "          \"test_start\": \"" << window.test_start << "\",\n"
                   << "          \"test_end\": \"" << window.test_end << "\",\n"
                   << "          \"train_rows\": " << window.train_rows << ",\n"
                   << "          \"validation_rows\": " << window.validation_rows << ",\n"
                   << "          \"test_rows\": " << window.test_rows << ",\n"
                   << "          \"selected_best_iteration\": "
                   << window.selected_best_iteration << ",\n"
                   << "          \"validation\": ";
            write_optional_metric_object(output, window.validation_metrics, "            ");
            output << ",\n          \"test\": ";
            write_optional_metric_object(output, window.test_metrics, "            ");
            output << "\n        }"
                   << (window_index + 1 == target.windows.size() ? "\n" : ",\n");
        }
        output << "      ]\n    }"
               << (target_index + 1 == results.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
}

void print_walk_forward_results(const std::vector<WalkForwardTargetResult>& results) {
    std::cout << "\nWalk-forward results (fixed default XGBoost parameters; no search)\n"
              << "Target | Test year | Validation accuracy | Validation log loss | Validation AUC | "
                 "Test accuracy | Test log loss | Test AUC\n";
    for (const auto& target : results) {
        for (const auto& window : target.windows) {
            const auto print_metric = [](const std::optional<arrakis::model::BinaryMetrics>& metrics,
                                         const auto selector) {
                return metrics.has_value() ? selector(*metrics) : std::numeric_limits<double>::quiet_NaN();
            };
            const auto validation_accuracy = print_metric(
                window.validation_metrics, [](const auto& metrics) { return metrics.accuracy; }
            );
            const auto validation_log_loss = print_metric(
                window.validation_metrics, [](const auto& metrics) { return metrics.log_loss; }
            );
            const auto validation_auc = print_metric(
                window.validation_metrics, [](const auto& metrics) { return metrics.roc_auc; }
            );
            const auto test_accuracy = print_metric(
                window.test_metrics, [](const auto& metrics) { return metrics.accuracy; }
            );
            const auto test_log_loss = print_metric(
                window.test_metrics, [](const auto& metrics) { return metrics.log_loss; }
            );
            const auto test_auc = print_metric(
                window.test_metrics, [](const auto& metrics) { return metrics.roc_auc; }
            );
            std::cout << target.target << " | " << window.test_year << " | "
                      << validation_accuracy << " | " << validation_log_loss << " | "
                      << validation_auc << " | " << test_accuracy << " | " << test_log_loss
                      << " | " << test_auc << '\n';
        }
        std::cout << "Promotion candidate: " << target.target << " = "
                  << (target.promotion_candidate ? "yes" : "no") << '\n';
    }
}

void run_walk_forward(const Options& options) {
    const auto base_dataset = arrakis::model::load_csv(options.input, "target_next_close_up");
    std::optional<PriceHistory> market;
    std::optional<PriceHistory> benchmark;
    if (!options.market_data.empty()) {
        market = load_price_history(options.market_data);
    }
    if (!options.benchmark_data.empty()) {
        benchmark = load_price_history(options.benchmark_data);
    }
    const auto results = run_walk_forward_targets(base_dataset, options, market, benchmark);
    write_walk_forward_results(
        options.walk_forward_output, options.input, options.feature_subset, results
    );
    print_walk_forward_results(results);
    std::cout << "Wrote walk-forward results to " << options.walk_forward_output << '\n';
}

void run_monthly_walk_forward(const Options& options) {
    auto dataset = load_target_dataset(options);
    dataset = add_requested_features(std::move(dataset), options);
    dataset = select_feature_subset(dataset, options.feature_subset);
    const auto result = run_monthly_evaluation(dataset, options, false);
    write_monthly_evaluation(
        options.monthly_walk_forward_output, options.input, options, result
    );
    print_monthly_result(result);
    std::cout << "Wrote monthly walk-forward results to "
              << options.monthly_walk_forward_output << '\n';
}

void write_ablation_summary(
    const std::filesystem::path& output_path,
    const std::filesystem::path& dataset_path,
    const Options& options,
    const std::vector<MonthlyEvaluationResult>& results
) {
    if (output_path.has_parent_path()) std::filesystem::create_directories(output_path.parent_path());
    std::ofstream output{output_path};
    if (!output) throw std::runtime_error{"Could not write ablation summary"};
    output << std::fixed << std::setprecision(6)
           << "{\n"
           << "  \"protocol\": {\n"
           << "    \"dataset_path\": \"" << dataset_path.string() << "\",\n"
           << "    \"target\": \"" << options.target << "\",\n"
           << "    \"profiles\": [";
    for (std::size_t index = 0; index < results.size(); ++index) {
        if (index > 0) output << ", ";
        output << '"' << results[index].feature_subset << '"';
    }
    output << "],\n"
           << "    \"volatility_features\": "
           << (options.volatility_features ? "true" : "false") << ",\n"
           << "    \"context_volatility_features\": "
           << (options.context_volatility_features ? "true" : "false") << ",\n"
           << "    \"selection_rule\": \"No tuning; additions require pooled AUC +0.01 and proper-scoring improvement\"\n"
           << "  },\n"
           << "  \"results\": [\n";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& result = results[index];
        const auto metrics = evaluate_if_binary(
            result.prediction_labels, result.prediction_probabilities
        );
        output << "    {\n"
               << "      \"profile\": \"" << result.feature_subset << "\",\n"
               << "      \"folds\": " << result.windows.size() << ",\n"
               << "      \"oos_predictions\": \"" << output_path.string() << '.'
               << result.feature_subset << ".oos_predictions.csv\",\n"
               << "      \"pooled_test\": ";
        write_optional_metric_object(output, metrics, "        ");
        output << "\n    }" << (index + 1 == results.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
}

void run_ablation(const Options& options) {
    auto base_dataset = load_target_dataset(options);
    base_dataset = add_requested_features(std::move(base_dataset), options);
    const std::vector<std::string_view> profiles = options.context_volatility_features
                                                       ? std::vector<std::string_view>{
                                                             "class-prior", "market",
                                                             "market-volatility",
                                                             "market-context-volatility", "logits-only",
                                                             "combined"}
                                                       : options.volatility_features
                                                       ? std::vector<std::string_view>{
                                                             "class-prior", "market",
                                                             "market-volatility", "logits-only",
                                                             "combined"}
                                                       : std::vector<std::string_view>{
                                                             "class-prior", "market", "logits-only",
                                                             "combined"};
    const auto market_dataset = select_feature_subset(base_dataset, "market");
    const auto volatility_dataset = options.volatility_features
                                        ? select_feature_subset(base_dataset, "market-volatility")
                                        : arrakis::model::Dataset{};
    const auto context_volatility_dataset = options.context_volatility_features
                                                ? select_feature_subset(
                                                      base_dataset, "market-context-volatility"
                                                  )
                                                : arrakis::model::Dataset{};
    const auto logits_dataset = select_feature_subset(base_dataset, "logits-only");
    const auto& combined_market_dataset = options.context_volatility_features
                                               ? context_volatility_dataset
                                               : options.volatility_features ? volatility_dataset
                                                                              : market_dataset;
    auto logits_combined_dataset = combined_market_dataset;
    logits_combined_dataset.feature_names.insert(
        logits_combined_dataset.feature_names.end(),
        logits_dataset.feature_names.begin(),
        logits_dataset.feature_names.end()
    );
    logits_combined_dataset.features.clear();
    logits_combined_dataset.features.reserve(
        base_dataset.row_count() * logits_combined_dataset.feature_names.size()
    );
    for (std::size_t row = 0; row < base_dataset.row_count(); ++row) {
        const auto market_begin = row * combined_market_dataset.feature_count();
        const auto news_begin = row * logits_dataset.feature_count();
        logits_combined_dataset.features.insert(
            logits_combined_dataset.features.end(),
            combined_market_dataset.features.begin() + static_cast<std::ptrdiff_t>(market_begin),
            combined_market_dataset.features.begin() + static_cast<std::ptrdiff_t>(
                market_begin + combined_market_dataset.feature_count()
            )
        );
        logits_combined_dataset.features.insert(
            logits_combined_dataset.features.end(),
            logits_dataset.features.begin() + static_cast<std::ptrdiff_t>(news_begin),
            logits_dataset.features.begin() + static_cast<std::ptrdiff_t>(
                news_begin + logits_dataset.feature_count()
            )
        );
    }
    std::vector<MonthlyEvaluationResult> results;
    results.reserve(profiles.size());
    for (const auto profile : profiles) {
        auto profile_options = options;
        profile_options.feature_subset = std::string{profile};
        profile_options.monthly_walk_forward_output =
            options.ablation_output.string() + "." + std::string{profile};
        const auto& profile_dataset = profile == "class-prior"
                                          ? base_dataset
                                          : profile == "market" ? market_dataset
                                          : profile == "market-volatility" ? volatility_dataset
                                          : profile == "market-context-volatility"
                                                ? context_volatility_dataset
                                          : profile == "logits-only" ? logits_dataset
                                                                      : logits_combined_dataset;
        auto result = run_monthly_evaluation(
            profile_dataset, profile_options, profile == "class-prior"
        );
        write_monthly_evaluation(
            profile_options.monthly_walk_forward_output,
            options.input,
            profile_options,
            result
        );
        print_monthly_result(result);
        results.push_back(std::move(result));
    }
    write_ablation_summary(options.ablation_output, options.input, options, results);
    std::cout << "Wrote ablation summary to " << options.ablation_output << '\n';
}

[[nodiscard]] PredictionDiagnostics summarize_predictions(
    const std::vector<float>& labels,
    const std::vector<float>& probabilities,
    const double threshold = 0.5
) {
    if (labels.empty() || labels.size() != probabilities.size()) {
        throw std::invalid_argument{
            "Labels and probabilities must be non-empty and equally sized for diagnostics"
        };
    }

    PredictionDiagnostics diagnostics{
        .minimum = std::numeric_limits<double>::infinity(),
        .maximum = -std::numeric_limits<double>::infinity(),
    };
    double sum = 0.0;
    double squared_deviation_sum = 0.0;
    for (std::size_t index = 0; index < labels.size(); ++index) {
        const auto label = static_cast<double>(labels[index]);
        const auto probability = static_cast<double>(probabilities[index]);
        if ((label != 0.0 && label != 1.0) || !std::isfinite(probability) ||
            probability < 0.0 || probability > 1.0) {
            throw std::invalid_argument{"Invalid label or probability for diagnostics"};
        }

        diagnostics.minimum = std::min(diagnostics.minimum, probability);
        diagnostics.maximum = std::max(diagnostics.maximum, probability);
        sum += probability;
        const auto bucket = std::min(
            static_cast<std::size_t>(probability * diagnostics.histogram.size()),
            diagnostics.histogram.size() - 1
        );
        ++diagnostics.histogram[bucket];

        const auto actual_positive = label == 1.0;
        const auto predicted_positive = probability >= threshold;
        if (actual_positive) {
            ++diagnostics.positive_labels;
        } else {
            ++diagnostics.negative_labels;
        }
        if (!actual_positive && !predicted_positive) {
            ++diagnostics.confusion_matrix.true_negative;
        } else if (!actual_positive && predicted_positive) {
            ++diagnostics.confusion_matrix.false_positive;
        } else if (actual_positive && !predicted_positive) {
            ++diagnostics.confusion_matrix.false_negative;
        } else {
            ++diagnostics.confusion_matrix.true_positive;
        }
    }

    const auto count = static_cast<double>(probabilities.size());
    diagnostics.mean = sum / count;
    for (const auto probability : probabilities) {
        const auto deviation = static_cast<double>(probability) - diagnostics.mean;
        squared_deviation_sum += deviation * deviation;
    }
    diagnostics.standard_deviation = std::sqrt(squared_deviation_sum / count);
    diagnostics.majority_label = diagnostics.positive_labels >= diagnostics.negative_labels ? 1 : 0;
    diagnostics.majority_accuracy = static_cast<double>(std::max(
        diagnostics.positive_labels,
        diagnostics.negative_labels
    )) / count;
    return diagnostics;
}

void write_prediction_diagnostics(
    std::ostream& output,
    const PredictionDiagnostics& diagnostics,
    const std::string_view indent
) {
    const std::string child_indent{indent};
    const std::string array_indent = child_indent + "  ";
    output << indent << "{\n"
           << array_indent << "\"min\": " << diagnostics.minimum << ",\n"
           << array_indent << "\"max\": " << diagnostics.maximum << ",\n"
           << array_indent << "\"mean\": " << diagnostics.mean << ",\n"
           << array_indent << "\"stddev\": " << diagnostics.standard_deviation << ",\n"
           << array_indent << "\"constant_prediction\": "
           << (diagnostics.is_constant() ? "true" : "false") << ",\n"
           << array_indent << "\"histogram_10_buckets\": [";
    for (std::size_t index = 0; index < diagnostics.histogram.size(); ++index) {
        if (index > 0) {
            output << ", ";
        }
        output << diagnostics.histogram[index];
    }
    output << "],\n"
           << array_indent << "\"confusion_matrix\": {\n"
           << array_indent << "  \"true_negative\": "
           << diagnostics.confusion_matrix.true_negative << ",\n"
           << array_indent << "  \"false_positive\": "
           << diagnostics.confusion_matrix.false_positive << ",\n"
           << array_indent << "  \"false_negative\": "
           << diagnostics.confusion_matrix.false_negative << ",\n"
           << array_indent << "  \"true_positive\": "
           << diagnostics.confusion_matrix.true_positive << "\n"
           << array_indent << "},\n"
           << array_indent << "\"positive_labels\": " << diagnostics.positive_labels << ",\n"
           << array_indent << "\"negative_labels\": " << diagnostics.negative_labels << ",\n"
           << array_indent << "\"majority_label\": " << diagnostics.majority_label << ",\n"
           << array_indent << "\"majority_accuracy\": " << diagnostics.majority_accuracy << '\n'
           << indent << "}";
}

void write_year_metrics(
    std::ostream& output,
    const std::vector<YearMetrics>& metrics,
    const std::string_view indent
) {
    output << indent << "[\n";
    for (std::size_t index = 0; index < metrics.size(); ++index) {
        const auto& year_metrics = metrics[index];
        output << indent << "  {\n"
               << indent << "    \"year\": " << year_metrics.year << ",\n"
               << indent << "    \"rows\": " << year_metrics.rows;
        if (year_metrics.metrics.has_value()) {
            output << ",\n";
            write_metric_object(output, *year_metrics.metrics, std::string{indent} + "    ");
        } else {
            output << ",\n" << indent << "    \"metrics\": null\n";
        }
        output << indent << "  }" << (index + 1 == metrics.size() ? "\n" : ",\n");
    }
    output << indent << "]";
}

void write_metrics(
    const std::filesystem::path& model_path,
    const Options& options,
    const arrakis::model::DatasetThreeWaySplit& split,
    const Hyperparameters& parameters,
    const TrainingResult& training,
    const arrakis::model::BinaryMetrics& validation_metrics,
    const arrakis::model::BinaryMetrics& test_metrics,
    const PredictionDiagnostics& validation_diagnostics,
    const PredictionDiagnostics& test_diagnostics,
    const std::vector<YearMetrics>& validation_year_metrics,
    const std::vector<YearMetrics>& test_year_metrics,
    const std::size_t search_candidates
) {
    auto metrics_path = model_path;
    metrics_path += ".metrics.json";
    std::ofstream output{metrics_path};
    if (!output) {
        throw std::runtime_error{"Could not write metrics: " + metrics_path.string()};
    }

    output << std::fixed << std::setprecision(6)
           << "{\n"
           << "  \"target\": \"" << options.target << "\",\n"
           << "  \"feature_subset\": \"" << options.feature_subset << "\",\n"
           << "  \"train_rows\": " << split.train.row_count() << ",\n"
           << "  \"validation_rows\": " << split.validation.row_count() << ",\n"
           << "  \"test_rows\": " << split.test.row_count() << ",\n"
           << "  \"evaluation_split\": \"test\",\n"
           << "  \"accuracy\": " << test_metrics.accuracy << ",\n"
           << "  \"log_loss\": " << test_metrics.log_loss << ",\n"
           << "  \"roc_auc\": " << test_metrics.roc_auc << ",\n"
           << "  \"positive_rate\": " << test_metrics.positive_rate << ",\n"
           << "  \"mean_probability\": " << test_metrics.mean_probability << ",\n"
           << "  \"selected_best_iteration\": " << training.best_iteration_index + 1 << ",\n"
           << "  \"rounds_run\": " << training.rounds_run << ",\n"
           << "  \"best_validation_logloss\": " << training.best_validation_logloss << ",\n"
           << "  \"early_stopping_rounds\": " << options.early_stopping_rounds << ",\n"
           << "  \"hyperparameter_search_candidates\": " << search_candidates << ",\n"
           << "  \"hyperparameters\": {\n"
           << "    \"eta\": " << parameters.eta << ",\n"
           << "    \"max_depth\": " << parameters.max_depth << ",\n"
           << "    \"min_child_weight\": " << parameters.min_child_weight << ",\n"
           << "    \"subsample\": " << parameters.subsample << ",\n"
           << "    \"colsample_bytree\": " << parameters.colsample_bytree << ",\n"
           << "    \"lambda\": " << parameters.lambda << ",\n"
           << "    \"alpha\": " << parameters.alpha << ",\n"
           << "    \"seed\": " << parameters.seed << "\n"
           << "  },\n"
           << "  \"validation_start\": \"" << split.validation.dates.front() << "\",\n"
           << "  \"validation_end\": \"" << split.validation.dates.back() << "\",\n"
           << "  \"test_start\": \"" << split.test.dates.front() << "\",\n"
           << "  \"test_end\": \"" << split.test.dates.back() << "\",\n"
           << "  \"validation\": {\n"
           << "    \"rows\": " << split.validation.row_count() << ",\n";
    write_metric_object(output, validation_metrics, "    ");
    output << "  },\n"
           << "  \"validation_prediction_diagnostics\": ";
    write_prediction_diagnostics(output, validation_diagnostics, "  ");
    output << ",\n"
           << "  \"test\": {\n"
           << "    \"rows\": " << split.test.row_count() << ",\n";
    write_metric_object(output, test_metrics, "    ");
    output << "  },\n"
           << "  \"test_prediction_diagnostics\": ";
    write_prediction_diagnostics(output, test_diagnostics, "  ");
    output << ",\n"
           << "  \"validation_by_year\": ";
    write_year_metrics(output, validation_year_metrics, "  ");
    output << ",\n  \"test_by_year\": ";
    write_year_metrics(output, test_year_metrics, "  ");
    output << "\n}\n";
}

[[nodiscard]] std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error{"Could not read dataset for checksum: " + path.string()};
    }
    const auto context = EVP_MD_CTX_new();
    if (context == nullptr || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        if (context != nullptr) {
            EVP_MD_CTX_free(context);
        }
        throw std::runtime_error{"Could not initialize dataset checksum"};
    }
    std::array<char, 1024 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 &&
            EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) != 1) {
            EVP_MD_CTX_free(context);
            throw std::runtime_error{"Could not update dataset checksum"};
        }
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0;
    if (EVP_DigestFinal_ex(context, digest.data(), &digest_length) != 1) {
        EVP_MD_CTX_free(context);
        throw std::runtime_error{"Could not finalize dataset checksum"};
    }
    EVP_MD_CTX_free(context);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < digest_length; ++index) {
        output << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    return output.str();
}

void write_test_predictions(
    const std::filesystem::path& model_path,
    const arrakis::model::Dataset& test,
    const std::vector<float>& probabilities
) {
    auto predictions_path = model_path;
    predictions_path += ".test_predictions.csv";
    std::ofstream output{predictions_path};
    if (!output) {
        throw std::runtime_error{
            "Could not write held-out predictions: " + predictions_path.string()
        };
    }
    output << "date,label,probability_positive_label\n";
    for (std::size_t index = 0; index < test.row_count(); ++index) {
        output << test.dates[index] << ',' << test.labels[index] << ',' << probabilities[index]
               << '\n';
    }
}

void write_search_results(
    const std::filesystem::path& model_path,
    const std::vector<SearchResult>& results
) {
    auto search_path = model_path;
    search_path += ".search.json";
    std::ofstream output{search_path};
    if (!output) {
        throw std::runtime_error{"Could not write search results: " + search_path.string()};
    }
    output << std::fixed << std::setprecision(6) << "{\n  \"selection_metric\": \"validation_logloss\",\n"
           << "  \"candidates\": [\n";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& result = results[index];
        const auto& parameters = result.hyperparameters;
        output << "    {\"validation_logloss\": " << result.validation_logloss
               << ", \"best_iteration\": " << result.best_iteration << ", \"eta\": "
               << parameters.eta << ", \"max_depth\": " << parameters.max_depth
               << ", \"min_child_weight\": " << parameters.min_child_weight
               << ", \"subsample\": " << parameters.subsample
               << ", \"colsample_bytree\": " << parameters.colsample_bytree
               << ", \"lambda\": " << parameters.lambda << ", \"alpha\": "
               << parameters.alpha << ", \"seed\": " << parameters.seed << "}"
               << (index + 1 == results.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
}

void write_manifest(
    const std::filesystem::path& model_path,
    const std::filesystem::path& dataset_path,
    const arrakis::model::Dataset& dataset,
    const arrakis::model::DatasetThreeWaySplit& split,
    const Options& options,
    const Hyperparameters& parameters,
    const TrainingResult& training
) {
    auto manifest_path = model_path;
    manifest_path += ".manifest.json";
    std::ofstream output{manifest_path};
    if (!output) {
        throw std::runtime_error{"Could not write model manifest: " + manifest_path.string()};
    }
    const auto has_embedding_columns = std::ranges::any_of(
        dataset.feature_names,
        [](const auto& name) { return name.starts_with("embedding_"); }
    );
    const std::string default_schema = options.feature_subset == "market"
                                           ? "xlk-market-features-v3"
                                           : options.feature_subset == "logits-only"
                                                 ? "xlk-news-logits-only-features-v3"
                                                 : options.feature_subset == "news"
                                                       ? (has_embedding_columns
                                                              ? "xlk-news-features-v1"
                                                              : "xlk-news-logits-only-features-v3")
                                                       : has_embedding_columns
                                                             ? "xlk-combined-features-v2"
                                                             : "xlk-logits-only-combined-features-v3";
    const auto schema_hash = options.feature_subset == "combined" &&
                                     std::getenv("ARRAKIS_FEATURE_SCHEMA_HASH") != nullptr
                                 ? std::string{std::getenv("ARRAKIS_FEATURE_SCHEMA_HASH")}
                                 : default_schema;
    output << std::fixed << std::setprecision(6)
           << "{\n"
           << "  \"model_id\": \"xlk-finbert-xgboost-v1\",\n"
           << "  \"model_type\": \"xgboost\",\n"
           << "  \"symbol\": \"XLK\",\n"
           << "  \"target\": \"" << options.target << "\",\n"
           << "  \"target_definition\": \""
           << (options.target == "target_high_volatility_next_day"
                   ? "absolute next-session open-to-close return above the trailing 20-session median absolute open-to-close return"
                   : "binary target supplied by the feature dataset or derived forward return")
           << "\",\n"
           << "  \"feature_subset\": \"" << options.feature_subset << "\",\n"
           << "  \"classification_threshold\": 0.5,\n"
           << "  \"finbert_version\": \""
           << (std::getenv("ARRAKIS_FINBERT_VERSION") == nullptr
                   ? "finbert-v1"
                   : std::getenv("ARRAKIS_FINBERT_VERSION"))
           << "\",\n"
           << "  \"tokenizer_version\": \""
           << (std::getenv("ARRAKIS_FINBERT_TOKENIZER_VERSION") == nullptr
                   ? "finbert-tokenizer-v1"
                   : std::getenv("ARRAKIS_FINBERT_TOKENIZER_VERSION"))
           << "\",\n"
           << "  \"aggregation_version\": \"" << default_schema << "\",\n"
           << "  \"feature_schema_hash\": \"" << schema_hash << "\",\n"
           << "  \"dataset_path\": \"" << dataset_path.string() << "\",\n"
           << "  \"dataset_sha256\": \"" << sha256_file(dataset_path) << "\",\n"
           << "  \"target_market_data\": \"" << options.market_data.string() << "\",\n"
           << "  \"target_benchmark_data\": \"" << options.benchmark_data.string() << "\",\n"
           << "  \"train_start\": \"" << split.train.dates.front() << "\",\n"
           << "  \"train_end\": \"" << split.train.dates.back() << "\",\n"
           << "  \"validation_start\": \"" << split.validation.dates.front() << "\",\n"
           << "  \"validation_end\": \"" << split.validation.dates.back() << "\",\n"
           << "  \"test_start\": \"" << split.test.dates.front() << "\",\n"
           << "  \"test_end\": \"" << split.test.dates.back() << "\",\n"
           << "  \"selected_best_iteration\": " << training.best_iteration_index + 1 << ",\n"
           << "  \"best_validation_logloss\": " << training.best_validation_logloss << ",\n"
           << "  \"early_stopping_rounds\": " << options.early_stopping_rounds << ",\n"
           << "  \"eta\": " << parameters.eta << ",\n"
           << "  \"max_depth\": " << parameters.max_depth << ",\n"
           << "  \"min_child_weight\": " << parameters.min_child_weight << ",\n"
           << "  \"subsample\": " << parameters.subsample << ",\n"
           << "  \"colsample_bytree\": " << parameters.colsample_bytree << ",\n"
           << "  \"lambda\": " << parameters.lambda << ",\n"
           << "  \"alpha\": " << parameters.alpha << ",\n"
           << "  \"seed\": " << parameters.seed << ",\n"
           << "  \"held_out_predictions\": \"" << model_path.filename().string()
           << ".test_predictions.csv\",\n"
           << "  \"feature_names\": [";
    for (std::size_t index = 0; index < dataset.feature_names.size(); ++index) {
        if (index > 0) {
            output << ", ";
        }
        output << '"' << dataset.feature_names[index] << '"';
    }
    output << "]\n}\n";
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        if (options.ablation) {
            run_ablation(options);
            return 0;
        }
        if (options.monthly_walk_forward) {
            run_monthly_walk_forward(options);
            return 0;
        }
        if (options.walk_forward) {
            run_walk_forward(options);
            return 0;
        }
        const auto alternative = alternative_target_spec(options.target);
        auto dataset = arrakis::model::load_csv(
            options.input,
            alternative.has_value() ? "target_next_close_up" : options.target
        );

        if (alternative.has_value()) {
            const auto market = load_price_history(options.market_data);
            std::optional<PriceHistory> benchmark;
            if (alternative->excess_return) {
                benchmark = load_price_history(options.benchmark_data);
            }
            dataset = apply_alternative_target(dataset, *alternative, market, benchmark);
        }
        dataset = add_requested_features(std::move(dataset), options);
        dataset = select_feature_subset(dataset, options.feature_subset);

        const auto legacy_split =
            arrakis::model::chronological_split(dataset, options.validation_fraction);
        const auto split = options.train_end.empty()
                               ? arrakis::model::DatasetThreeWaySplit{
                                     .train = legacy_split.train,
                                     .validation = legacy_split.validation,
                                     .test = legacy_split.validation,
                                 }
                               : arrakis::model::chronological_split_by_dates(
                                     dataset,
                                     options.train_end,
                                     options.validation_end,
                                     options.test_end
                                 );

        std::cout << "Loaded " << dataset.row_count() << " rows and "
                  << dataset.feature_count() << " " << options.feature_subset << " features\n"
                  << "Train: " << split.train.dates.front() << " to "
                  << split.train.dates.back() << " (" << split.train.row_count() << " rows)\n"
                  << "Validation: " << split.validation.dates.front() << " to "
                  << split.validation.dates.back() << " (" << split.validation.row_count()
                  << " rows)\n"
                  << "Test: " << split.test.dates.front() << " to " << split.test.dates.back()
                  << " (" << split.test.row_count() << " rows)\n";

        const DMatrix train_matrix{split.train};
        const DMatrix validation_matrix{split.validation};
        Hyperparameters selected_parameters = options.hyperparameters;
        std::vector<SearchResult> search_results;
        if (options.hyperparameter_search) {
            search_results = run_hyperparameter_search(
                train_matrix.get(),
                validation_matrix.get(),
                options,
                selected_parameters
            );
            std::cout << "Validation-only search evaluated " << search_results.size()
                      << " configurations; selected validation logloss "
                      << search_results.front().validation_logloss << "\n";
        }

        const DMatrix test_matrix{split.test};
        auto checkpoint = options.model_output;
        checkpoint += ".best.tmp.ubj";
        const auto training = train_booster(
            train_matrix.get(),
            validation_matrix.get(),
            selected_parameters,
            options.rounds,
            options.early_stopping_rounds,
            checkpoint,
            true
        );
        const std::vector<DMatrixHandle> final_matrices{
            train_matrix.get(), validation_matrix.get()
        };
        Booster final_booster{final_matrices};
        final_booster.load(checkpoint);
        std::error_code checkpoint_error;
        std::filesystem::remove(checkpoint, checkpoint_error);

        const auto validation_predictions = final_booster.predict(validation_matrix.get());
        const auto test_predictions = final_booster.predict(test_matrix.get());
        const auto validation_metrics = arrakis::model::evaluate_binary_classifier(
            split.validation.labels, validation_predictions
        );
        const auto test_metrics = arrakis::model::evaluate_binary_classifier(
            split.test.labels, test_predictions
        );
        const auto validation_diagnostics = summarize_predictions(
            split.validation.labels, validation_predictions
        );
        const auto test_diagnostics = summarize_predictions(split.test.labels, test_predictions);
        const auto validation_year_metrics = evaluate_by_year(
            split.validation, validation_predictions
        );
        const auto test_year_metrics = evaluate_by_year(split.test, test_predictions);

        final_booster.save(options.model_output);
        write_metrics(
            options.model_output,
            options,
            split,
            selected_parameters,
            training,
            validation_metrics,
            test_metrics,
            validation_diagnostics,
            test_diagnostics,
            validation_year_metrics,
            test_year_metrics,
            search_results.size()
        );
        write_test_predictions(options.model_output, split.test, test_predictions);
        if (!search_results.empty()) {
            write_search_results(options.model_output, search_results);
        }
        write_manifest(
            options.model_output,
            options.input,
            dataset,
            split,
            options,
            selected_parameters,
            training
        );

        std::cout << std::fixed << std::setprecision(4)
                  << "\nSelected best iteration: " << training.best_iteration_index + 1
                  << " (validation logloss " << training.best_validation_logloss << ")\n"
                  << "Validation metrics\n"
                  << "  Accuracy: " << validation_metrics.accuracy << '\n'
                  << "  Log loss: " << validation_metrics.log_loss << '\n'
                  << "  ROC AUC:  " << validation_metrics.roc_auc << '\n'
                  << "Held-out test metrics\n"
                  << "  Accuracy: " << test_metrics.accuracy << '\n'
                  << "  Log loss: " << test_metrics.log_loss << '\n'
                  << "  ROC AUC:  " << test_metrics.roc_auc << '\n'
                  << "  Actual positive rate: " << test_metrics.positive_rate << '\n'
                  << "  Mean probability:     " << test_metrics.mean_probability << '\n'
                  << "  Probability min/max:  " << test_diagnostics.minimum << "/"
                  << test_diagnostics.maximum << '\n'
                  << "  Probability stddev:   " << test_diagnostics.standard_deviation << '\n'
                  << "  Majority baseline:    " << test_diagnostics.majority_accuracy
                  << " (label " << test_diagnostics.majority_label << ")\n"
                  << "  Confusion matrix TN/FP/FN/TP: "
                  << test_diagnostics.confusion_matrix.true_negative << '/'
                  << test_diagnostics.confusion_matrix.false_positive << '/'
                  << test_diagnostics.confusion_matrix.false_negative << '/'
                  << test_diagnostics.confusion_matrix.true_positive << '\n'
                  << "  Constant prediction:  "
                  << (test_diagnostics.is_constant() ? "yes" : "no") << '\n'
                  << "Saved model: " << options.model_output << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Training failed: " << error.what() << '\n';
        return 1;
    }
}
