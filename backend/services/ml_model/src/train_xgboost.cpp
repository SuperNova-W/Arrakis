#include "arrakis/model/dataset.hpp"
#include "arrakis/model/metrics.hpp"

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
};

struct Options final {
    std::filesystem::path input;
    std::filesystem::path model_output{"artifacts/xlk_news_xgboost.json"};
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
    std::vector<double> closes;
    std::unordered_map<std::string, std::size_t> index_by_date;
};

struct TargetSpec final {
    int horizon{};
    bool excess_return{false};
    bool high_volatility{false};
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
        history.closes.push_back(std::stod(fields[5]));
    }

    if (history.dates.empty()) {
        throw std::runtime_error{"Market history has no usable rows: " + path.string()};
    }
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
            market.closes[return_index] / market.closes[return_index - 1] - 1.0
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

        double forward_return =
            market.closes[future_index] / market.closes[market_it->second] - 1.0;
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
            const auto threshold = trailing_median_absolute_return(market, market_it->second);
            positive_target = std::abs(forward_return) > threshold;
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

[[nodiscard]] arrakis::model::Dataset select_feature_subset(
    const arrakis::model::Dataset& source,
    const std::string_view subset
) {
    if (subset == "combined") {
        return source;
    }
    constexpr std::size_t market_features = 9;
    constexpr std::size_t news_features = 27;
    if (source.feature_count() != market_features + news_features) {
        throw std::invalid_argument{
            "market/news feature subsets require exactly 36 input features"
        };
    }

    std::size_t first_feature = 0;
    std::size_t feature_count = 0;
    if (subset == "market") {
        feature_count = market_features;
    } else if (subset == "news") {
        first_feature = market_features;
        feature_count = news_features;
    } else {
        throw std::invalid_argument{"--feature-subset must be combined, market, or news"};
    }

    arrakis::model::Dataset result;
    result.dates = source.dates;
    result.labels = source.labels;
    result.feature_names.assign(
        source.feature_names.begin() + static_cast<std::ptrdiff_t>(first_feature),
        source.feature_names.begin() +
            static_cast<std::ptrdiff_t>(first_feature + feature_count)
    );
    result.features.reserve(result.row_count() * feature_count);
    for (std::size_t row = 0; row < source.row_count(); ++row) {
        const auto feature_begin = row * source.feature_count() + first_feature;
        const auto feature_end = feature_begin + feature_count;
        result.features.insert(
            result.features.end(),
            source.features.begin() + static_cast<std::ptrdiff_t>(feature_begin),
            source.features.begin() + static_cast<std::ptrdiff_t>(feature_end)
        );
    }
    return result;
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
        } else if (argument == "--target") {
            options.target = require_value();
        } else if (argument == "--feature-subset") {
            options.feature_subset = require_value();
        } else if (argument == "--market-data") {
            options.market_data = require_value();
        } else if (argument == "--benchmark-data") {
            options.benchmark_data = require_value();
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
                << "                                  excess_return_3d_up, excess_return_5d_up\n"
                << "  --feature-subset <name>         combined, market, or news (default: combined)\n"
                << "  --market-data <path>             XLK history for alternative targets\n"
                << "  --benchmark-data <path>          SPY history for excess-return targets\n"
                << "  --model-output <path>            XGBoost model output path\n"
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
    booster.set_parameter("seed", "42");
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
           << "    \"alpha\": " << parameters.alpha << "\n"
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
               << parameters.alpha << "}"
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
    const auto default_schema = options.feature_subset == "combined"
                                    ? "xlk-combined-features-v1"
                                    : "xlk-" + options.feature_subset + "-features-v1";
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
                   ? "absolute next-day return above trailing 20-day median absolute return"
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
