#include "arrakis/model/dataset.hpp"
#include "arrakis/model/metrics.hpp"
#include "arrakis/model/tcn.hpp"

#include <mlx/mlx.h>

#include <openssl/evp.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace {

using mlx::core::array;
using arrakis::model::Dataset;
using arrakis::model::TCNConfig;

struct Options final {
    std::filesystem::path input{"data/fnspid/normalized/xlk_combined_features.csv"};
    std::filesystem::path output_dir{"artifacts/tcn_xlk"};
    std::string train_end{"2020-12-31"};
    std::string validation_end{"2022-12-31"};
    std::string test_end{"2023-12-28"};
    std::size_t epochs{300};
    std::size_t patience{35};
    std::uint64_t seed{42};
    double learning_rate{0.001};
    double beta1{0.9};
    double beta2{0.999};
    double epsilon{1.0e-8};
    std::size_t sequence_length{30};
    std::size_t hidden_channels{16};
    std::string feature_profile{"full"};
    std::string normalization{"channel"};
    std::string calibration{"none"};
    double weight_decay{0.0};
    double gradient_clip{0.0};
    std::size_t warmup_epochs{0};
    std::string device{"gpu"};
    bool walk_forward{false};
};

struct Standardization final {
    std::vector<float> means;
    std::vector<float> scales;
};

struct SequenceSet final {
    std::vector<float> values;
    std::vector<float> labels;
    std::vector<std::string> dates;
    std::size_t sequence_length{};
    std::size_t feature_count{};

    [[nodiscard]] std::size_t row_count() const noexcept { return labels.size(); }
};

struct Fold final {
    std::string name;
    std::string train_end;
    std::string validation_end;
    std::string test_end;
};

struct TrainResult final {
    std::vector<array> parameters;
    std::size_t best_epoch{};
    double best_validation_loss{std::numeric_limits<double>::infinity()};
};

struct PlattCalibrator final {
    double slope{1.0};
    double intercept{0.0};
};

struct Evaluation final {
    arrakis::model::BinaryMetrics metrics{};
    std::vector<float> logits;
    std::vector<float> probabilities;
    double threshold{0.5};
};

void usage() {
    std::cout
        << "Usage: arrakis-train-tcn [options]\n"
        << "  --input <csv>              Combined XLK feature CSV\n"
        << "  --output-dir <directory>  Artifact directory\n"
        << "  --train-end <date>        Inclusive training end date\n"
        << "  --validation-end <date>   Inclusive validation end date\n"
        << "  --test-end <date>         Inclusive test end date\n"
        << "  --epochs <n>               Maximum epochs\n"
        << "  --patience <n>             Early-stopping patience\n"
        << "  --learning-rate <value>   Adam learning rate\n"
        << "  --window <n>               Causal sequence length\n"
        << "  --hidden-channels <n>     TCN channel width\n"
        << "  --feature-profile full|compact  Input feature profile\n"
        << "  --normalization channel|none  Hidden channel normalization\n"
        << "  --calibration none|platt  Validation-only probability calibration\n"
        << "  --weight-decay <value>    AdamW decoupled weight decay\n"
        << "  --gradient-clip <value>   Global gradient-norm clip\n"
        << "  --warmup-epochs <n>       Linear learning-rate warmup\n"
        << "  --seed <n>                 Deterministic seed\n"
        << "  --device gpu              GPU-only MLX device (default: gpu)\n"
        << "  --walk-forward             Train the three documented expanding folds\n";
}

template <typename T>
T parse_number(const std::string& value, const char* option) {
    try {
        if constexpr (std::is_integral_v<T>) {
            std::size_t consumed = 0;
            const auto parsed = std::stoull(value, &consumed);
            if (consumed != value.size()) throw std::invalid_argument{"trailing characters"};
            return static_cast<T>(parsed);
        } else {
            std::size_t consumed = 0;
            const auto parsed = std::stod(value, &consumed);
            if (consumed != value.size()) throw std::invalid_argument{"trailing characters"};
            return static_cast<T>(parsed);
        }
    } catch (const std::exception&) {
        throw std::invalid_argument{std::string{"Invalid value for "} + option + ": " + value};
    }
}

Options parse_options(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string option{argv[index]};
        if (option == "--help") {
            usage();
            std::exit(EXIT_SUCCESS);
        }
        if (option == "--walk-forward") {
            options.walk_forward = true;
            continue;
        }
        if (index + 1 >= argc) throw std::invalid_argument{"Missing value for " + option};
        const std::string value{argv[++index]};
        if (option == "--input") options.input = value;
        else if (option == "--output-dir") options.output_dir = value;
        else if (option == "--train-end") options.train_end = value;
        else if (option == "--validation-end") options.validation_end = value;
        else if (option == "--test-end") options.test_end = value;
        else if (option == "--epochs") options.epochs = parse_number<std::size_t>(value, option.c_str());
        else if (option == "--patience") options.patience = parse_number<std::size_t>(value, option.c_str());
        else if (option == "--learning-rate") options.learning_rate = parse_number<double>(value, option.c_str());
        else if (option == "--window") options.sequence_length = parse_number<std::size_t>(value, option.c_str());
        else if (option == "--hidden-channels") options.hidden_channels = parse_number<std::size_t>(value, option.c_str());
        else if (option == "--feature-profile") options.feature_profile = value;
        else if (option == "--normalization") options.normalization = value;
        else if (option == "--calibration") options.calibration = value;
        else if (option == "--weight-decay") options.weight_decay = parse_number<double>(value, option.c_str());
        else if (option == "--gradient-clip") options.gradient_clip = parse_number<double>(value, option.c_str());
        else if (option == "--warmup-epochs") options.warmup_epochs = parse_number<std::size_t>(value, option.c_str());
        else if (option == "--seed") options.seed = parse_number<std::uint64_t>(value, option.c_str());
        else if (option == "--device") options.device = value;
        else throw std::invalid_argument{"Unknown option: " + option};
    }
    if (options.epochs == 0 || options.patience == 0 || options.sequence_length < 3 ||
        options.hidden_channels == 0 || !(options.learning_rate > 0.0) ||
        options.weight_decay < 0.0 || options.gradient_clip < 0.0 ||
        (options.feature_profile != "full" && options.feature_profile != "compact") ||
        (options.normalization != "channel" && options.normalization != "none") ||
        (options.calibration != "none" && options.calibration != "platt") ||
        options.device != "gpu") {
        throw std::invalid_argument{"Invalid TCN training options"};
    }
    return options;
}

std::size_t first_after(const Dataset& dataset, const std::string& date) {
    const auto found = std::ranges::upper_bound(dataset.dates, date);
    return static_cast<std::size_t>(std::distance(dataset.dates.begin(), found));
}

Dataset prepare_dataset(const Dataset& raw, const std::string& profile) {
    if (profile == "full") return raw;

    static const std::vector<std::string> compact_names{
        "ret_1",
        "ret_3",
        "ret_6",
        "volatility_6",
        "volume_mean_6",
        "rel_volume",
        "rsi_14",
        "spy_ret_1",
        "sector_spy_diff",
        "article_count",
        "positive_proportion",
        "negative_proportion",
        "sentiment_stddev",
        "time_decayed_sentiment",
        "max_positive_sentiment",
        "max_negative_sentiment",
        "news_coverage",
        "news_freshness_hours"
    };
    std::unordered_map<std::string, std::size_t> source_indices;
    for (std::size_t index = 0; index < raw.feature_names.size(); ++index) {
        source_indices.emplace(raw.feature_names[index], index);
    }

    Dataset compact;
    compact.dates = raw.dates;
    compact.labels = raw.labels;
    compact.feature_names.reserve(compact_names.size());
    std::vector<std::size_t> indices;
    indices.reserve(compact_names.size());
    for (const auto& name : compact_names) {
        const auto found = source_indices.find(name);
        if (found == source_indices.end()) {
            throw std::runtime_error{"Compact TCN feature is missing from dataset: " + name};
        }
        indices.push_back(found->second);
        if (name == "article_count") compact.feature_names.emplace_back("log1p_article_count");
        else if (name == "volume_mean_6") compact.feature_names.emplace_back("log1p_volume_mean_6");
        else if (name == "news_freshness_hours") compact.feature_names.emplace_back("news_freshness_hours_capped");
        else compact.feature_names.push_back(name);
    }

    const auto raw_features = raw.feature_count();
    compact.features.reserve(raw.row_count() * indices.size());
    for (std::size_t row = 0; row < raw.row_count(); ++row) {
        for (std::size_t feature = 0; feature < indices.size(); ++feature) {
            const auto name = compact_names[feature];
            auto value = raw.features[row * raw_features + indices[feature]];
            if (name == "article_count" || name == "volume_mean_6") {
                value = std::log1p(std::max(value, 0.0F));
            } else if (name == "news_freshness_hours") {
                value = std::min(std::max(value, 0.0F), 168.0F);
            }
            if (!std::isfinite(value)) {
                throw std::runtime_error{"Compact TCN feature became non-finite: " + name};
            }
            compact.features.push_back(value);
        }
    }
    return compact;
}

Standardization fit_standardization(const Dataset& dataset, const std::size_t rows) {
    if (rows == 0 || rows > dataset.row_count()) throw std::invalid_argument{"Training range is empty"};
    const auto features = dataset.feature_count();
    Standardization result{std::vector<float>(features), std::vector<float>(features, 0.0F)};
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t feature = 0; feature < features; ++feature) {
            const auto value = dataset.features[row * features + feature];
            if (!std::isfinite(value)) throw std::runtime_error{"Dataset contains a non-finite feature"};
            result.means[feature] += value;
        }
    }
    for (auto& value : result.means) value /= static_cast<float>(rows);
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t feature = 0; feature < features; ++feature) {
            const auto centered = dataset.features[row * features + feature] - result.means[feature];
            result.scales[feature] += centered * centered;
        }
    }
    for (auto& value : result.scales) {
        value = std::sqrt(value / static_cast<float>(rows));
        if (!(value > 1.0e-6F) || !std::isfinite(value)) value = 1.0F;
    }
    return result;
}

SequenceSet make_sequences(
    const Dataset& dataset,
    const Standardization& standardization,
    const std::size_t begin,
    const std::size_t end,
    const std::size_t sequence_length
) {
    if (begin >= end || end > dataset.row_count()) throw std::invalid_argument{"Sequence range is empty"};
    if (sequence_length == 0 || sequence_length > end) throw std::invalid_argument{"Sequence length is invalid for range"};
    const auto features = dataset.feature_count();
    SequenceSet result;
    result.sequence_length = sequence_length;
    result.feature_count = features;
    const auto first_end = std::max(sequence_length - 1, begin);
    if (first_end >= end) throw std::invalid_argument{"Sequence range has no usable rows"};
    result.values.reserve((end - first_end) * sequence_length * features);
    result.labels.reserve(end - first_end);
    result.dates.reserve(end - first_end);
    for (std::size_t row = first_end; row < end; ++row) {
        for (std::size_t offset = row + 1 - sequence_length; offset <= row; ++offset) {
            for (std::size_t feature = 0; feature < features; ++feature) {
                const auto value = dataset.features[offset * features + feature];
                result.values.push_back((value - standardization.means[feature]) / standardization.scales[feature]);
            }
        }
        result.labels.push_back(dataset.labels[row]);
        result.dates.push_back(dataset.dates[row]);
    }
    return result;
}

array sequence_array(const SequenceSet& data) {
    return array(
        data.values.begin(),
        mlx::core::Shape{
            static_cast<int>(data.row_count()),
            static_cast<int>(data.sequence_length),
            static_cast<int>(data.feature_count)
        },
        mlx::core::float32
    );
}

array label_array(const SequenceSet& data) {
    return array(
        data.labels.begin(),
        mlx::core::Shape{static_cast<int>(data.row_count())},
        mlx::core::float32
    );
}

array binary_loss(const TCNConfig& config, const std::vector<array>& parameters, const array& inputs, const array& labels) {
    const auto logits = arrakis::model::tcn_logits(config, parameters, inputs);
    const auto zero = array(0.0F);
    return mlx::core::mean(
        mlx::core::maximum(logits, zero) - logits * labels +
        mlx::core::log1p(mlx::core::exp(-mlx::core::abs(logits)))
    );
}

std::string device_name() {
    const auto& info = mlx::core::device_info();
    const auto found = info.find("device_name");
    if (found == info.end()) return "unknown";
    if (const auto* value = std::get_if<std::string>(&found->second)) return *value;
    return "unknown";
}

void evaluate_arrays(std::vector<array>& values) {
    mlx::core::eval(values);
}

float sigmoid_host(const double value) {
    const auto bounded = std::clamp(value, -60.0, 60.0);
    if (bounded >= 0.0) {
        const auto negative = std::exp(-bounded);
        return static_cast<float>(1.0 / (1.0 + negative));
    }
    const auto positive = std::exp(bounded);
    return static_cast<float>(positive / (1.0 + positive));
}

Evaluation evaluate(
    const TCNConfig& config,
    const std::vector<array>& parameters,
    const SequenceSet& data,
    const array& inputs,
    const double threshold = 0.5,
    const PlattCalibrator* calibrator = nullptr
) {
    auto logits = arrakis::model::tcn_logits(config, parameters, inputs);
    logits.eval();
    const auto* raw = logits.data<float>();
    std::vector<float> logit_values(raw, raw + data.row_count());
    std::vector<float> values;
    values.reserve(data.row_count());
    for (const auto logit : logit_values) {
        const auto calibrated_logit = calibrator == nullptr
            ? static_cast<double>(logit)
            : calibrator->slope * static_cast<double>(logit) + calibrator->intercept;
        values.push_back(sigmoid_host(calibrated_logit));
    }
    return Evaluation{
        .metrics = arrakis::model::evaluate_binary_classifier(data.labels, values, threshold),
        .logits = std::move(logit_values),
        .probabilities = std::move(values),
        .threshold = threshold
    };
}

PlattCalibrator fit_platt_calibrator(
    const Evaluation& uncalibrated_validation,
    const SequenceSet& validation
) {
    if (uncalibrated_validation.logits.size() != validation.row_count()) {
        throw std::invalid_argument{"Platt calibration inputs have mismatched row counts"};
    }
    const auto logits = array(
        uncalibrated_validation.logits.begin(),
        mlx::core::Shape{static_cast<int>(validation.row_count())},
        mlx::core::float32
    );
    const auto labels = label_array(validation);
    array log_slope = array(0.0F);
    array intercept = array(0.0F);
    std::vector<int> parameter_indices{0, 1};
    const auto loss_function = [logits, labels](const std::vector<array>& parameters) {
        const auto calibrated_logits = mlx::core::exp(parameters[0]) * logits + parameters[1];
        const auto zero = array(0.0F);
        const auto logistic_loss = mlx::core::mean(
            mlx::core::maximum(calibrated_logits, zero) - calibrated_logits * labels +
            mlx::core::log1p(mlx::core::exp(-mlx::core::abs(calibrated_logits)))
        );
        const auto regularization = 1.0e-4F *
            (mlx::core::square(parameters[0]) + mlx::core::square(parameters[1]));
        return std::vector<array>{logistic_loss + regularization};
    };
    const auto value_and_gradient = mlx::core::value_and_grad(loss_function, parameter_indices);
    std::vector<array> parameters{log_slope, intercept};
    std::vector<array> first_moments{mlx::core::zeros_like(log_slope), mlx::core::zeros_like(intercept)};
    std::vector<array> second_moments{mlx::core::zeros_like(log_slope), mlx::core::zeros_like(intercept)};
    constexpr double beta1 = 0.9;
    constexpr double beta2 = 0.999;
    constexpr double epsilon = 1.0e-8;
    constexpr double learning_rate = 0.05;
    for (std::size_t epoch = 1; epoch <= 250; ++epoch) {
        auto result = value_and_gradient(parameters);
        auto gradients = result.second;
        for (std::size_t index = 0; index < gradients.size(); ++index) {
            first_moments[index] = beta1 * first_moments[index] +
                (1.0 - beta1) * gradients[index];
            second_moments[index] = beta2 * second_moments[index] +
                (1.0 - beta2) * mlx::core::square(gradients[index]);
            const auto bias_correction_1 = 1.0 - std::pow(beta1, static_cast<double>(epoch));
            const auto bias_correction_2 = 1.0 - std::pow(beta2, static_cast<double>(epoch));
            const auto step_size = learning_rate * std::sqrt(bias_correction_2) /
                bias_correction_1;
            parameters[index] = parameters[index] - step_size * first_moments[index] /
                (mlx::core::sqrt(second_moments[index]) + epsilon);
        }
    }
    mlx::core::eval(parameters);
    const auto log_slope_value = std::clamp(static_cast<double>(parameters[0].item<float>()), -8.0, 8.0);
    const auto intercept_value = std::clamp(static_cast<double>(parameters[1].item<float>()), -8.0, 8.0);
    return PlattCalibrator{
        .slope = std::exp(log_slope_value),
        .intercept = intercept_value
    };
}

double select_accuracy_threshold(
    const std::vector<float>& labels,
    const std::vector<float>& probabilities
) {
    double best_threshold = 0.5;
    std::size_t best_correct = 0;
    for (std::size_t step = 5; step <= 95; ++step) {
        const auto threshold = static_cast<double>(step) / 100.0;
        std::size_t correct = 0;
        for (std::size_t index = 0; index < labels.size(); ++index) {
            correct += static_cast<std::size_t>(
                (probabilities[index] >= threshold) == (labels[index] == 1.0F)
            );
        }
        if (correct > best_correct || (correct == best_correct &&
            std::abs(threshold - 0.5) < std::abs(best_threshold - 0.5))) {
            best_correct = correct;
            best_threshold = threshold;
        }
    }
    return best_threshold;
}

TrainResult train_network(
    const TCNConfig& config,
    const SequenceSet& train,
    const SequenceSet& validation,
    const Options& options
) {
    const auto train_inputs = sequence_array(train);
    const auto train_labels = label_array(train);
    const auto validation_inputs = sequence_array(validation);
    auto network = arrakis::model::TCNNetwork::random(config, options.seed);
    auto parameters = network.parameters();
    auto best_parameters = parameters;
    std::vector<array> first_moments;
    std::vector<array> second_moments;
    first_moments.reserve(parameters.size());
    second_moments.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        first_moments.push_back(mlx::core::zeros_like(parameter));
        second_moments.push_back(mlx::core::zeros_like(parameter));
    }

    const auto parameter_count = parameters.size();
    std::vector<int> parameter_indices;
    parameter_indices.reserve(parameter_count);
    for (std::size_t index = 0; index < parameter_count; ++index) {
        parameter_indices.push_back(static_cast<int>(index));
    }
    const auto loss_function = [config, parameter_count](const std::vector<array>& inputs) {
        std::vector<array> parameters(inputs.begin(), inputs.begin() + static_cast<std::ptrdiff_t>(parameter_count));
        return std::vector<array>{binary_loss(config, parameters, inputs[parameter_count], inputs[parameter_count + 1])};
    };
    const auto value_and_gradient = mlx::core::value_and_grad(loss_function, parameter_indices);

    double best_validation_loss = std::numeric_limits<double>::infinity();
    std::size_t best_epoch = 0;
    std::size_t stale_epochs = 0;
    for (std::size_t epoch = 1; epoch <= options.epochs; ++epoch) {
        std::vector<array> inputs(parameters.begin(), parameters.end());
        inputs.push_back(train_inputs);
        inputs.push_back(train_labels);
        const auto result = value_and_gradient(inputs);
        auto loss = result.first.front();
        auto gradients = result.second;
        loss.eval();
        const auto train_loss = static_cast<double>(loss.item<float>());
        if (options.gradient_clip > 0.0) {
            auto gradient_norm_squared = array(0.0F);
            for (const auto& gradient : gradients) {
                gradient_norm_squared = gradient_norm_squared + mlx::core::sum(mlx::core::square(gradient));
            }
            gradient_norm_squared.eval();
            const auto gradient_norm = std::sqrt(std::max(0.0F, gradient_norm_squared.item<float>()));
            if (gradient_norm > options.gradient_clip) {
                const auto clip_scale = static_cast<float>(options.gradient_clip / gradient_norm);
                for (auto& gradient : gradients) gradient = gradient * clip_scale;
            }
        }
        const auto step = static_cast<double>(epoch);
        const auto bias_correction_1 = 1.0 - std::pow(options.beta1, step);
        const auto bias_correction_2 = 1.0 - std::pow(options.beta2, step);
        double effective_learning_rate = options.learning_rate;
        if (options.warmup_epochs > 0 && epoch <= options.warmup_epochs) {
            effective_learning_rate *= static_cast<double>(epoch) /
                static_cast<double>(options.warmup_epochs);
        } else if (options.epochs > options.warmup_epochs) {
            const auto schedule_epoch = static_cast<double>(epoch - options.warmup_epochs);
            const auto schedule_length = static_cast<double>(options.epochs - options.warmup_epochs);
            const auto progress = std::min(1.0, schedule_epoch / schedule_length);
            effective_learning_rate *= 0.5 * (1.0 + std::cos(std::numbers::pi * progress));
        }
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            first_moments[index] = options.beta1 * first_moments[index] + (1.0 - options.beta1) * gradients[index];
            second_moments[index] = options.beta2 * second_moments[index] + (1.0 - options.beta2) * mlx::core::square(gradients[index]);
            const auto step_size = effective_learning_rate * std::sqrt(bias_correction_2) / bias_correction_1;
            parameters[index] = parameters[index] - step_size * first_moments[index] /
                (mlx::core::sqrt(second_moments[index]) + options.epsilon) -
                effective_learning_rate * options.weight_decay * parameters[index];
        }
        evaluate_arrays(parameters);

        auto validation_loss = binary_loss(config, parameters, validation_inputs, label_array(validation));
        validation_loss.eval();
        const auto validation_value = static_cast<double>(validation_loss.item<float>());
        if (epoch == 1 || epoch % 10 == 0 || validation_value < best_validation_loss) {
            std::cout << "epoch=" << epoch << " train_logloss=" << std::fixed << std::setprecision(6)
                      << train_loss << " validation_logloss=" << validation_value << '\n';
        }
        if (validation_value + 1.0e-7 < best_validation_loss) {
            best_validation_loss = validation_value;
            best_parameters = parameters;
            best_epoch = epoch;
            stale_epochs = 0;
        } else {
            ++stale_epochs;
            if (stale_epochs >= options.patience) break;
        }
    }
    return TrainResult{
        .parameters = std::move(best_parameters),
        .best_epoch = best_epoch,
        .best_validation_loss = best_validation_loss
    };
}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error{"Could not open dataset for hashing: " + path.string()};
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        if (context != nullptr) EVP_MD_CTX_free(context);
        throw std::runtime_error{"Could not initialize SHA-256"};
    }
    std::vector<char> buffer(1U << 16U);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) != 1) {
            EVP_MD_CTX_free(context);
            throw std::runtime_error{"Could not hash dataset"};
        }
    }
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digest_length = 0;
    if (EVP_DigestFinal_ex(context, digest, &digest_length) != 1) {
        EVP_MD_CTX_free(context);
        throw std::runtime_error{"Could not finalize dataset hash"};
    }
    EVP_MD_CTX_free(context);
    std::ostringstream output;
    for (unsigned int index = 0; index < digest_length; ++index) {
        output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(digest[index]);
    }
    return output.str();
}

void write_predictions(const std::filesystem::path& path, const SequenceSet& data, const Evaluation& evaluation) {
    std::ofstream output(path);
    if (!output) throw std::runtime_error{"Could not write predictions: " + path.string()};
    output << "date,label,probability_positive_return\n";
    for (std::size_t index = 0; index < data.row_count(); ++index) {
        output << data.dates[index] << ',' << data.labels[index] << ',' << std::setprecision(9)
               << evaluation.probabilities[index] << '\n';
    }
}

void write_metrics(
    const std::filesystem::path& path,
    const Evaluation& validation,
    const Evaluation& test,
    const TrainResult& training,
    const Fold& fold,
    const SequenceSet& train,
    const SequenceSet& validation_data,
    const SequenceSet& test_data,
    const Options& options,
    const PlattCalibrator& calibrator
) {
    std::ofstream output(path);
    if (!output) throw std::runtime_error{"Could not write TCN metrics: " + path.string()};
    const auto emit = [&](const arrakis::model::BinaryMetrics& metrics) {
        output << "{\"accuracy\": " << metrics.accuracy
               << ", \"log_loss\": " << metrics.log_loss
               << ", \"roc_auc\": " << metrics.roc_auc
               << ", \"positive_rate\": " << metrics.positive_rate
               << ", \"mean_probability\": " << metrics.mean_probability << '}';
    };
    output << std::fixed << std::setprecision(6)
           << "{\n  \"model_id\": \"xlk-tcn-v2\",\n"
           << "  \"train_end\": \"" << fold.train_end << "\",\n"
           << "  \"validation_end\": \"" << fold.validation_end << "\",\n"
           << "  \"test_end\": \"" << fold.test_end << "\",\n"
           << "  \"best_epoch\": " << training.best_epoch << ",\n"
           << "  \"best_validation_log_loss\": " << training.best_validation_loss << ",\n"
           << "  \"classification_threshold\": " << validation.threshold << ",\n"
           << "  \"normalization\": \"" << options.normalization << "\",\n"
           << "  \"calibration\": \"" << options.calibration << "\",\n"
           << "  \"calibration_slope\": " << calibrator.slope << ",\n"
           << "  \"calibration_intercept\": " << calibrator.intercept << ",\n"
           << "  \"train_rows\": " << train.row_count() << ",\n"
           << "  \"validation_rows\": " << validation_data.row_count() << ",\n"
           << "  \"test_rows\": " << test_data.row_count() << ",\n"
           << "  \"validation\": ";
    emit(validation.metrics);
    output << ",\n  \"test\": ";
    emit(test.metrics);
    output << "\n}\n";
}

void write_manifest(
    const std::filesystem::path& path,
    const Dataset& dataset,
    const Standardization& standardization,
    const TCNConfig& config,
    const Options& options,
    const Fold& fold,
    const TrainResult& training,
    const double classification_threshold,
    const std::string& device,
    const PlattCalibrator& calibrator
) {
    std::ofstream output(path);
    if (!output) throw std::runtime_error{"Could not write TCN manifest: " + path.string()};
    output << std::fixed << std::setprecision(8)
           << "{\n"
           << "  \"model_id\": \"xlk-tcn-v2\",\n"
           << "  \"model_type\": \"causal_temporal_convolutional_network\",\n"
           << "  \"framework\": \"mlx-cpp\",\n"
           << "  \"device\": \"" << device << "\",\n"
           << "  \"symbol\": \"XLK\",\n"
           << "  \"target\": \"target_next_close_up\",\n"
           << "  \"classification_threshold\": " << classification_threshold << ",\n"
           << "  \"normalization\": \"" << options.normalization << "\",\n"
           << "  \"hidden_channel_normalization\": "
           << (config.use_channel_normalization ? "true" : "false") << ",\n"
           << "  \"calibration\": \"" << options.calibration << "\",\n"
           << "  \"calibration_slope\": " << calibrator.slope << ",\n"
           << "  \"calibration_intercept\": " << calibrator.intercept << ",\n"
           << "  \"feature_profile\": \"" << options.feature_profile << "\",\n"
           << "  \"feature_schema_hash\": \"xlk-tcn-" << options.feature_profile << "-v2\",\n"
           << "  \"dataset_path\": \"" << options.input.string() << "\",\n"
           << "  \"dataset_sha256\": \"" << sha256_file(options.input) << "\",\n"
           << "  \"train_end\": \"" << fold.train_end << "\",\n"
           << "  \"validation_end\": \"" << fold.validation_end << "\",\n"
           << "  \"test_end\": \"" << fold.test_end << "\",\n"
           << "  \"sequence_length\": " << config.sequence_length << ",\n"
           << "  \"hidden_channels\": " << config.hidden_channels << ",\n"
           << "  \"kernel_size\": " << config.kernel_size << ",\n"
           << "  \"dilations\": [";
    for (std::size_t index = 0; index < config.dilations.size(); ++index) {
        if (index != 0) output << ", ";
        output << config.dilations[index];
    }
    output << "],\n"
           << "  \"seed\": " << options.seed << ",\n"
           << "  \"learning_rate\": " << options.learning_rate << ",\n"
           << "  \"weight_decay\": " << options.weight_decay << ",\n"
           << "  \"gradient_clip\": " << options.gradient_clip << ",\n"
           << "  \"warmup_epochs\": " << options.warmup_epochs << ",\n"
           << "  \"best_epoch\": " << training.best_epoch << ",\n"
           << "  \"feature_names\": [";
    for (std::size_t index = 0; index < dataset.feature_names.size(); ++index) {
        if (index != 0) output << ", ";
        output << '"' << dataset.feature_names[index] << '"';
    }
    output << "],\n  \"normalization_mean\": [";
    for (std::size_t index = 0; index < standardization.means.size(); ++index) {
        if (index != 0) output << ", ";
        output << standardization.means[index];
    }
    output << "],\n  \"normalization_scale\": [";
    for (std::size_t index = 0; index < standardization.scales.size(); ++index) {
        if (index != 0) output << ", ";
        output << standardization.scales[index];
    }
    output << "]\n}\n";
}

struct FoldResult final {
    Evaluation validation;
    Evaluation test;
    std::filesystem::path output_dir;
};

FoldResult run_fold(
    const Dataset& dataset,
    const TCNConfig& config,
    const Options& options,
    const Fold& fold,
    const std::filesystem::path& output_dir,
    const std::string& device
) {
    const auto train_boundary = first_after(dataset, fold.train_end);
    const auto validation_boundary = first_after(dataset, fold.validation_end);
    const auto test_boundary = first_after(dataset, fold.test_end);
    // The one-day target for the last row in a split is realized in the next
    // partition. Purge each split's final feature row so no target horizon
    // crosses a train/validation or validation/test boundary.
    const auto train_end = train_boundary == 0 ? 0 : train_boundary - 1;
    const auto validation_begin = train_boundary;
    const auto validation_end = validation_boundary == 0 ? 0 : validation_boundary - 1;
    const auto test_begin = validation_boundary;
    const auto test_end = test_boundary;
    if (train_end == 0 || train_boundary >= validation_boundary ||
        validation_end <= validation_begin || validation_boundary >= test_boundary ||
        test_end > dataset.row_count()) {
        throw std::runtime_error{"Fold boundaries do not cover the ordered dataset: " + fold.name};
    }
    const auto standardization = fit_standardization(dataset, train_end);
    const auto train = make_sequences(dataset, standardization, 0, train_end, config.sequence_length);
    const auto validation = make_sequences(
        dataset, standardization, validation_begin, validation_end, config.sequence_length);
    const auto test = make_sequences(dataset, standardization, test_begin, test_end, config.sequence_length);
    std::cout << "fold=" << fold.name << " train_rows=" << train.row_count()
              << " validation_rows=" << validation.row_count() << " test_rows=" << test.row_count()
              << " device=" << device << '\n';
    const auto training = train_network(config, train, validation, options);
    const auto validation_inputs = sequence_array(validation);
    const auto test_inputs = sequence_array(test);
    const auto uncalibrated_validation = evaluate(
        config, training.parameters, validation, validation_inputs);
    PlattCalibrator calibrator;
    const PlattCalibrator* calibrator_ptr = nullptr;
    if (options.calibration == "platt") {
        calibrator = fit_platt_calibrator(uncalibrated_validation, validation);
        calibrator_ptr = &calibrator;
        std::cout << "fold=" << fold.name << " platt_slope=" << calibrator.slope
                  << " platt_intercept=" << calibrator.intercept << '\n';
    }
    const auto calibrated_validation = evaluate(
        config, training.parameters, validation, validation_inputs, 0.5, calibrator_ptr);
    const auto threshold = select_accuracy_threshold(
        validation.labels, calibrated_validation.probabilities);
    const auto validation_evaluation = evaluate(
        config, training.parameters, validation, validation_inputs, threshold, calibrator_ptr);
    const auto test_evaluation = evaluate(
        config, training.parameters, test, test_inputs, threshold, calibrator_ptr);
    std::cout << "fold=" << fold.name << " best_epoch=" << training.best_epoch
              << " validation_accuracy=" << validation_evaluation.metrics.accuracy
              << " validation_auc=" << validation_evaluation.metrics.roc_auc
              << " threshold=" << threshold
              << " test_accuracy=" << test_evaluation.metrics.accuracy
              << " test_auc=" << test_evaluation.metrics.roc_auc << '\n';

    std::filesystem::create_directories(output_dir);
    arrakis::model::TCNNetwork network{config, training.parameters};
    network.save(output_dir / "weights.safetensors");
    write_metrics(
        output_dir / "metrics.json",
        validation_evaluation,
        test_evaluation,
        training,
        fold,
        train,
        validation,
        test,
        options,
        calibrator
    );
    write_predictions(output_dir / "test_predictions.csv", test, test_evaluation);
    write_manifest(
        output_dir / "manifest.json",
        dataset,
        standardization,
        config,
        options,
        fold,
        training,
        test_evaluation.threshold,
        device,
        calibrator
    );
    return FoldResult{validation_evaluation, test_evaluation, output_dir};
}

void write_walk_forward_summary(
    const std::filesystem::path& path,
    const std::vector<std::pair<Fold, FoldResult>>& results
) {
    std::ofstream output(path);
    if (!output) throw std::runtime_error{"Could not write walk-forward summary: " + path.string()};
    output << std::fixed << std::setprecision(6)
           << "{\n  \"model_id\": \"xlk-tcn-v2\",\n  \"folds\": [\n";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& [fold, result] = results[index];
        output << "    {\"name\": \"" << fold.name << "\", \"train_end\": \""
               << fold.train_end << "\", \"validation_end\": \"" << fold.validation_end
               << "\", \"test_end\": \"" << fold.test_end
               << "\", \"classification_threshold\": " << result.test.threshold
               << ", \"test_accuracy\": "
               << result.test.metrics.accuracy << ", \"test_log_loss\": "
               << result.test.metrics.log_loss << ", \"test_roc_auc\": "
               << result.test.metrics.roc_auc << "}"
               << (index + 1 == results.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto device = mlx::core::Device::gpu;
        if (!mlx::core::is_available(device)) {
            throw std::runtime_error{"Requested MLX device is unavailable"};
        }
        mlx::core::set_default_device(device);
        const auto device_label = device_name();
        std::cout << "MLX device=" << device_label << " backend=" << options.device << '\n';
        const auto raw_dataset = arrakis::model::load_csv(options.input, "target_next_close_up");
        const auto dataset = prepare_dataset(raw_dataset, options.feature_profile);
        if (options.feature_profile == "full" && dataset.feature_count() != 36) {
            throw std::runtime_error{"Full TCN profile requires the 36-feature XLK combined schema"};
        }
        TCNConfig config;
        config.feature_count = dataset.feature_count();
        config.sequence_length = options.sequence_length;
        config.hidden_channels = options.hidden_channels;
        config.use_channel_normalization = options.normalization == "channel";
        if (options.feature_profile == "compact") config.dilations = {1, 2, 4};
        std::vector<Fold> folds;
        if (options.walk_forward) {
            folds = {
                {"2021_test", "2019-12-31", "2020-12-31", "2021-12-31"},
                {"2022_test", "2020-12-31", "2021-12-31", "2022-12-31"},
                {"2023_test", "2021-12-31", "2022-12-31", "2023-12-28"}
            };
        } else {
            folds = {{"fixed_holdout", options.train_end, options.validation_end, options.test_end}};
        }

        std::vector<std::pair<Fold, FoldResult>> results;
        for (const auto& fold : folds) {
            const auto output_dir = options.walk_forward ? options.output_dir / ("fold_" + fold.name) : options.output_dir;
            results.emplace_back(
                fold,
                run_fold(dataset, config, options, fold, output_dir, device_label)
            );
        }
        if (options.walk_forward) write_walk_forward_summary(options.output_dir / "walk_forward.json", results);
        std::cout << "TCN training complete: " << options.output_dir << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "TCN training failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
