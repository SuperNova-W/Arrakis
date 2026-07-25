#include "arrakis/model/dataset.hpp"
#include "arrakis/model/metrics.hpp"

#include <xgboost/c_api.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
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
            "Predicting validation data"
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

  private:
    BoosterHandle handle_{nullptr};
};

struct Options final {
    std::filesystem::path input;
    std::filesystem::path model_output{"artifacts/xgboost_baseline.json"};
    std::string target{"target_up_5d"};
    double validation_fraction{0.2};
    int rounds{75};
};

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
        } else if (argument == "--validation-fraction") {
            options.validation_fraction = std::stod(std::string{require_value()});
        } else if (argument == "--rounds") {
            options.rounds = std::stoi(std::string{require_value()});
        } else if (argument == "--help") {
            std::cout
                << "Usage: arrakis-train-xgboost --input <features.csv> [options]\n\n"
                << "Options:\n"
                << "  --target <name>                 Target column (default: target_up_5d)\n"
                << "  --model-output <path>           JSON model output path\n"
                << "  --validation-fraction <0-0.5>   Most recent fraction for validation\n"
                << "  --rounds <count>                Boosting rounds (default: 75)\n";
            std::exit(0);
        } else {
            throw std::invalid_argument{"Unknown argument: " + std::string{argument}};
        }
    }

    if (options.input.empty()) {
        throw std::invalid_argument{"--input is required"};
    }
    if (options.rounds <= 0) {
        throw std::invalid_argument{"--rounds must be positive"};
    }
    return options;
}

void write_metrics(
    const std::filesystem::path& model_path,
    const arrakis::model::DatasetSplit& split,
    const arrakis::model::BinaryMetrics& metrics
) {
    auto metrics_path = model_path;
    metrics_path += ".metrics.json";
    std::ofstream output{metrics_path};
    if (!output) {
        throw std::runtime_error{"Could not write metrics: " + metrics_path.string()};
    }

    output << std::fixed << std::setprecision(6)
           << "{\n"
           << "  \"train_rows\": " << split.train.row_count() << ",\n"
           << "  \"validation_rows\": " << split.validation.row_count() << ",\n"
           << "  \"validation_start\": \"" << split.validation.dates.front() << "\",\n"
           << "  \"validation_end\": \"" << split.validation.dates.back() << "\",\n"
           << "  \"accuracy\": " << metrics.accuracy << ",\n"
           << "  \"log_loss\": " << metrics.log_loss << ",\n"
           << "  \"roc_auc\": " << metrics.roc_auc << ",\n"
           << "  \"positive_rate\": " << metrics.positive_rate << ",\n"
           << "  \"mean_probability\": " << metrics.mean_probability << "\n"
           << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto dataset = arrakis::model::load_csv(options.input, options.target);
        const auto split =
            arrakis::model::chronological_split(dataset, options.validation_fraction);

        std::cout << "Loaded " << dataset.row_count() << " rows and "
                  << dataset.feature_count() << " features\n"
                  << "Train: " << split.train.dates.front() << " to "
                  << split.train.dates.back() << " (" << split.train.row_count() << " rows)\n"
                  << "Validation: " << split.validation.dates.front() << " to "
                  << split.validation.dates.back() << " ("
                  << split.validation.row_count() << " rows)\n";

        const DMatrix train_matrix{split.train};
        const DMatrix validation_matrix{split.validation};
        const std::vector<DMatrixHandle> matrices{
            train_matrix.get(), validation_matrix.get()
        };
        const std::vector<const char*> matrix_names{"train", "validation"};

        Booster booster{matrices};
        booster.set_parameter("objective", "binary:logistic");
        booster.set_parameter("eval_metric", "logloss");
        booster.set_parameter("tree_method", "hist");
        booster.set_parameter("device", "cpu");
        booster.set_parameter("eta", "0.05");
        booster.set_parameter("max_depth", "3");
        booster.set_parameter("min_child_weight", "1");
        booster.set_parameter("subsample", "0.8");
        booster.set_parameter("colsample_bytree", "0.8");
        booster.set_parameter("lambda", "1.0");
        booster.set_parameter("seed", "42");

        for (int iteration = 0; iteration < options.rounds; ++iteration) {
            booster.update(iteration, train_matrix.get());
            if (iteration == 0 || (iteration + 1) % 10 == 0 ||
                iteration + 1 == options.rounds) {
                std::cout << booster.evaluate(iteration, matrices, matrix_names) << '\n';
            }
        }

        const auto probabilities = booster.predict(validation_matrix.get());
        const auto metrics = arrakis::model::evaluate_binary_classifier(
            split.validation.labels, probabilities
        );
        booster.save(options.model_output);
        write_metrics(options.model_output, split, metrics);

        std::cout << std::fixed << std::setprecision(4)
                  << "\nValidation metrics\n"
                  << "  Accuracy: " << metrics.accuracy << '\n'
                  << "  Log loss: " << metrics.log_loss << '\n'
                  << "  ROC AUC:  " << metrics.roc_auc << '\n'
                  << "  Actual positive rate: " << metrics.positive_rate << '\n'
                  << "  Mean probability:     " << metrics.mean_probability << '\n'
                  << "Saved model: " << options.model_output << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Training failed: " << error.what() << '\n';
        return 1;
    }
}
