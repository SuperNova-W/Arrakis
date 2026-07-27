#include "arrakis/model/sector_ml.hpp"

#include <xgboost/c_api.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <regex>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arrakis::model {
namespace {

void check_xgboost(const int result, const std::string_view operation) {
    if (result != 0) {
        throw std::runtime_error{std::string{operation} + " failed: " + XGBGetLastError()};
    }
}

class DMatrix final {
  public:
    explicit DMatrix(const Dataset& dataset) {
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
            XGDMatrixSetFloatInfo(handle_, "label", dataset.labels.data(), static_cast<bst_ulong>(dataset.labels.size())),
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

    explicit Booster(const std::filesystem::path& path) {
        check_xgboost(XGBoosterCreate(nullptr, 0, &handle_), "Creating booster");
        check_xgboost(XGBoosterLoadModel(handle_, path.string().c_str()), "Loading booster model");
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
        check_xgboost(
            XGBoosterSetParam(handle_, std::string{name}.c_str(), std::string{value}.c_str()),
            "Setting parameter"
        );
    }

    void update(const int iteration, const DMatrixHandle train) {
        check_xgboost(
            XGBoosterUpdateOneIter(handle_, iteration, train),
            "Training iteration"
        );
    }

    [[nodiscard]] std::vector<float> predict(const DMatrixHandle matrix) const {
        constexpr std::string_view config = R"({"type":0,"training":false,"iteration_begin":0,"iteration_end":0,"strict_shape":true})";
        const bst_ulong* shape = nullptr;
        bst_ulong dimensions = 0;
        const float* predictions = nullptr;

        check_xgboost(
            XGBoosterPredictFromDMatrix(handle_, matrix, config.data(), &shape, &dimensions, &predictions),
            "Predicting"
        );
        if (dimensions == 0 || shape == nullptr) {
            throw std::runtime_error{"XGBoost returned an invalid prediction shape"};
        }
        std::size_t count = 1;
        for (bst_ulong index = 0; index < dimensions; ++index) {
            count *= static_cast<std::size_t>(shape[index]);
        }
        return {predictions, predictions + count};
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

std::string to_lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string build_metadata_json(
    const FeatureConfiguration& config,
    const Dataset& dataset,
    const std::filesystem::path& model_path
) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(6)
           << "{\n"
           << "  \"model_id\": \"xgb_" << to_lower(config.target_symbol) << "\",\n"
           << "  \"target_symbol\": \"" << config.target_symbol << "\",\n"
           << "  \"model_version\": \"v001\",\n"
           << "  \"feature_version\": \"" << config.feature_version << "\",\n"
           << "  \"target_definition\": \"future_log_return\",\n"
           << "  \"prediction_horizon\": " << config.prediction_horizon_bars << ",\n"
           << "  \"bar_interval\": " << config.bar_interval_minutes << ",\n"
           << "  \"training_start\": \"" << (config.training_start.empty() ? "n/a" : config.training_start) << "\",\n"
           << "  \"training_end\": \"" << (dataset.dates.empty() ? "n/a" : dataset.dates.back()) << "\",\n"
           << "  \"validation_start\": \"" << (config.validation_start.empty() ? "n/a" : config.validation_start) << "\",\n"
           << "  \"validation_end\": \"" << (dataset.dates.empty() ? "n/a" : dataset.dates.back()) << "\",\n"
           << "  \"test_start\": \"" << (config.test_start.empty() ? "n/a" : config.test_start) << "\",\n"
           << "  \"test_end\": \"" << (dataset.dates.empty() ? "n/a" : dataset.dates.back()) << "\",\n"
           << "  \"xgboost_version\": \"unknown\",\n"
           << "  \"compiler_version\": \"unknown\",\n"
           << "  \"git_commit\": \"unknown\",\n"
           << "  \"random_seed\": " << config.random_seed << ",\n"
           << "  \"hyperparameters\": {\n"
           << "    \"objective\": \"" << config.objective << "\",\n"
           << "    \"eval_metric\": \"" << config.eval_metric << "\",\n"
           << "    \"max_depth\": " << config.max_depth << ",\n"
           << "    \"eta\": " << config.eta << ",\n"
           << "    \"subsample\": " << config.subsample << ",\n"
           << "    \"colsample_bytree\": " << config.colsample_bytree << ",\n"
           << "    \"gamma\": " << config.gamma << ",\n"
           << "    \"lambda\": " << config.lambda << ",\n"
           << "    \"alpha\": " << config.alpha << ",\n"
           << "    \"max_rounds\": " << config.max_rounds << ",\n"
           << "    \"early_stopping_rounds\": " << config.early_stopping_rounds << "\n"
           << "  },\n"
           << "  \"model_path\": \"" << model_path.string() << "\"\n"
           << "}\n";
    return output.str();
}

std::string build_feature_schema_json(const std::vector<std::string>& feature_names) {
    std::ostringstream output;
    output << "{\n  \"features\": [";
    for (std::size_t index = 0; index < feature_names.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        output << "\"" << feature_names[index] << "\"";
    }
    output << "],\n  \"feature_version\": \"sector-features-v1\"\n}\n";
    return output.str();
}

}  // namespace

struct SectorXGBoostModel::Impl final {
    std::string model_id;
    std::string feature_version;
    std::vector<std::string> feature_names;
    std::filesystem::path model_path;
    std::string target_symbol;
    std::string schema_hash;
    std::string model_version;
    int prediction_horizon{0};
    std::unique_ptr<Booster> booster;
};

SectorFeatureBuilder::SectorFeatureBuilder(FeatureConfiguration config) : config_(std::move(config)) {}

const std::vector<std::string>& SectorFeatureBuilder::feature_names() const noexcept {
    return feature_names_;
}

Dataset SectorFeatureBuilder::build(const std::vector<BarRecord>& bars) const {
    if (bars.empty()) {
        throw std::invalid_argument{"No bars available for feature generation"};
    }

    std::map<std::string, std::vector<BarRecord>> by_symbol;
    for (const auto& bar : bars) {
        by_symbol[bar.symbol].push_back(bar);
    }

    if (by_symbol.find(config_.target_symbol) == by_symbol.end()) {
        throw std::invalid_argument{"Target symbol not present in input dataset"};
    }

    std::vector<BarRecord> target_bars = by_symbol.at(config_.target_symbol);
    std::ranges::sort(target_bars, [](const BarRecord& left, const BarRecord& right) {
        return left.timestamp < right.timestamp;
    });

    if (target_bars.size() < static_cast<std::size_t>(config_.lookback_window + config_.prediction_horizon_bars + 3)) {
        throw std::invalid_argument{"Not enough history to engineer features"};
    }

    std::vector<std::string> feature_names;
    feature_names.reserve(10);
    feature_names.push_back("ret_1");
    feature_names.push_back("ret_3");
    feature_names.push_back("ret_6");
    feature_names.push_back("volatility_6");
    feature_names.push_back("volume_mean_6");
    feature_names.push_back("rel_volume");
    feature_names.push_back("rsi_14");
    feature_names.push_back("spy_ret_1");
    feature_names.push_back("spy_ret_3");
    feature_names.push_back("sector_spy_diff");
    feature_names_ = feature_names;

    Dataset dataset;
    dataset.feature_names = feature_names;
    dataset.dates.reserve(target_bars.size());
    dataset.features.reserve(target_bars.size() * feature_names.size());
    dataset.labels.reserve(target_bars.size());

    const auto find_symbol_bars = [&](const std::string& symbol) -> const std::vector<BarRecord>& {
        const auto it = by_symbol.find(symbol);
        if (it == by_symbol.end()) {
            throw std::invalid_argument{"Required symbol is missing: " + symbol};
        }
        return it->second;
    };

    const auto& spy = find_symbol_bars("SPY");
    const auto get_index = [](const std::vector<BarRecord>& source, const std::string& timestamp) {
        for (std::size_t index = 0; index < source.size(); ++index) {
            if (source[index].timestamp == timestamp) {
                return index;
            }
        }
        return static_cast<std::size_t>(-1);
    };

    const auto min_row = static_cast<std::size_t>(config_.lookback_window + config_.prediction_horizon_bars + 1);
    for (std::size_t row = min_row; row + static_cast<std::size_t>(config_.prediction_horizon_bars) < target_bars.size(); ++row) {
        const auto& current = target_bars[row];
        const auto current_index = get_index(target_bars, current.timestamp);
        if (current_index == static_cast<std::size_t>(-1)) {
            continue;
        }

        const auto& prev_1 = target_bars[row - 1];
        const auto& prev_3 = target_bars[row - 3];
        const auto& prev_6 = target_bars[row - 6];
        const auto return_1 = (current.close / prev_1.close) - 1.0;
        const auto return_3 = (current.close / prev_3.close) - 1.0;
        const auto return_6 = (current.close / prev_6.close) - 1.0;
        const auto volatility_6 = std::sqrt(std::max(1e-8, std::abs(return_6) + 1e-8));

        double volume_sum = 0.0;
        for (std::size_t offset = 0; offset < 6; ++offset) {
            volume_sum += target_bars[row - offset].volume;
        }
        const auto volume_mean_6 = volume_sum / 6.0;
        const auto rel_volume = current.volume / std::max(1e-8, volume_mean_6);
        const auto rsi_14 = std::clamp(50.0 + 10.0 * return_1, 0.0, 100.0);

        const auto spy_index = std::min(current_index, spy.size() - 1);
        const auto& spy_bar = spy[spy_index];
        const auto spy_prev = spy[std::max<std::size_t>(0, spy_index > 0 ? spy_index - 1 : 0)];
        const auto spy_ret_1 = (spy_bar.close / spy_prev.close) - 1.0;
        const auto sector_spy_diff = return_1 - spy_ret_1;

        dataset.dates.push_back(current.timestamp);
        dataset.features.push_back(static_cast<float>(return_1));
        dataset.features.push_back(static_cast<float>(return_3));
        dataset.features.push_back(static_cast<float>(return_6));
        dataset.features.push_back(static_cast<float>(volatility_6));
        dataset.features.push_back(static_cast<float>(volume_mean_6));
        dataset.features.push_back(static_cast<float>(rel_volume));
        dataset.features.push_back(static_cast<float>(rsi_14));
        dataset.features.push_back(static_cast<float>(spy_ret_1));
        dataset.features.push_back(static_cast<float>(sector_spy_diff));

        const auto future_close = target_bars[row + static_cast<std::size_t>(config_.prediction_horizon_bars)].close;
        const auto future_return = std::log(future_close / current.close);
        dataset.labels.push_back(static_cast<float>(future_return));
    }

    return dataset;
}

DatasetSplit SectorFeatureBuilder::build_split(
    const Dataset& dataset,
    const double validation_fraction,
    const int purge_bars,
    const int embargo_bars
) const {
    if (!(validation_fraction > 0.0 && validation_fraction < 0.5)) {
        throw std::invalid_argument{"Validation fraction must be between 0 and 0.5"};
    }
    if (dataset.row_count() < 10) {
        throw std::invalid_argument{"Dataset is too small to split"};
    }

    const auto validation_rows = static_cast<std::size_t>(
        std::ceil(static_cast<double>(dataset.row_count()) * validation_fraction)
    );
    const auto split_index = dataset.row_count() - validation_rows;
    const auto purge_index = std::max<std::size_t>(0, split_index - static_cast<std::size_t>(purge_bars));
    const auto embargo_index = std::max<std::size_t>(0, purge_index - static_cast<std::size_t>(embargo_bars));

    Dataset train;
    train.feature_names = dataset.feature_names;
    Dataset validation;
    validation.feature_names = dataset.feature_names;

    for (std::size_t index = 0; index < dataset.row_count(); ++index) {
        if (index < embargo_index) {
            continue;
        }
        if (index < purge_index) {
            train.dates.push_back(dataset.dates[index]);
            const auto feature_begin = index * dataset.feature_count();
            const auto feature_end = (index + 1) * dataset.feature_count();
            train.features.insert(train.features.end(), dataset.features.begin() + static_cast<std::ptrdiff_t>(feature_begin), dataset.features.begin() + static_cast<std::ptrdiff_t>(feature_end));
            train.labels.push_back(dataset.labels[index]);
        } else if (index >= split_index) {
            validation.dates.push_back(dataset.dates[index]);
            const auto feature_begin = index * dataset.feature_count();
            const auto feature_end = (index + 1) * dataset.feature_count();
            validation.features.insert(validation.features.end(), dataset.features.begin() + static_cast<std::ptrdiff_t>(feature_begin), dataset.features.begin() + static_cast<std::ptrdiff_t>(feature_end));
            validation.labels.push_back(dataset.labels[index]);
        }
    }

    return DatasetSplit{.train = std::move(train), .validation = std::move(validation)};
}

SectorXGBoostTrainer::SectorXGBoostTrainer(FeatureConfiguration config) : config_(std::move(config)) {}

std::filesystem::path SectorXGBoostTrainer::train(
    const Dataset& training_data,
    const std::filesystem::path& output_dir
) const {
    if (training_data.row_count() < 4) {
        throw std::invalid_argument{"Training dataset is too small"};
    }

    const auto model_path = output_dir / "model.ubj";
    const auto metadata_path = output_dir / "metadata.json";
    const auto schema_path = output_dir / "feature_schema.json";

    std::filesystem::create_directories(output_dir);

    DMatrix train_matrix{training_data};

    Booster booster{{train_matrix.get()}};
    booster.set_parameter("seed", std::to_string(config_.random_seed));
    booster.set_parameter("objective", config_.objective);
    booster.set_parameter("eval_metric", config_.eval_metric);
    booster.set_parameter("max_depth", std::to_string(config_.max_depth));
    booster.set_parameter("eta", std::to_string(config_.eta));
    booster.set_parameter("subsample", std::to_string(config_.subsample));
    booster.set_parameter("colsample_bytree", std::to_string(config_.colsample_bytree));
    booster.set_parameter("gamma", std::to_string(config_.gamma));
    booster.set_parameter("lambda", std::to_string(config_.lambda));
    booster.set_parameter("alpha", std::to_string(config_.alpha));
    booster.set_parameter("nthread", std::to_string(config_.nthread));
    booster.set_parameter("tree_method", config_.tree_method);

    for (int iteration = 0; iteration < config_.max_rounds; ++iteration) {
        booster.update(iteration, train_matrix.get());
    }
    booster.save(model_path);

    std::ofstream metadata{metadata_path};
    metadata << build_metadata_json(config_, training_data, model_path);
    std::ofstream schema{schema_path};
    schema << build_feature_schema_json(training_data.feature_names);

    return model_path;
}

SectorXGBoostModel::SectorXGBoostModel() : impl_(std::make_unique<Impl>()) {}

SectorXGBoostModel::~SectorXGBoostModel() = default;

SectorXGBoostModel::SectorXGBoostModel(SectorXGBoostModel&&) noexcept = default;
SectorXGBoostModel& SectorXGBoostModel::operator=(SectorXGBoostModel&&) noexcept = default;

void SectorXGBoostModel::load(
    const std::filesystem::path& model_path,
    const std::filesystem::path& metadata_path,
    const std::filesystem::path& schema_path
) {
    if (!std::filesystem::exists(model_path)) {
        throw std::runtime_error{"Model file missing: " + model_path.string()};
    }
    if (!std::filesystem::exists(metadata_path)) {
        throw std::runtime_error{"Metadata file missing: " + metadata_path.string()};
    }
    if (!std::filesystem::exists(schema_path)) {
        throw std::runtime_error{"Feature schema file missing: " + schema_path.string()};
    }

    std::ifstream metadata_input{metadata_path};
    std::string contents((std::istreambuf_iterator<char>(metadata_input)), std::istreambuf_iterator<char>());
    const auto string_field = [&](const std::string& key, const std::string& fallback) {
        const std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\""); std::smatch match;
        return std::regex_search(contents, match, pattern) ? match[1].str() : fallback;
    };
    impl_->model_id = string_field("model_id", "");
    impl_->target_symbol = string_field("target_symbol", "");
    impl_->model_version = string_field("model_version", "v001");
    const std::regex horizon_pattern("\\\"prediction_horizon\\\"\\s*:\\s*([0-9]+)"); std::smatch horizon_match;
    if (std::regex_search(contents, horizon_match, horizon_pattern)) impl_->prediction_horizon = std::stoi(horizon_match[1].str());
    impl_->feature_version = string_field("feature_version", "");
    if (impl_->model_id.empty() || impl_->target_symbol.empty() || impl_->feature_version.empty()) throw std::runtime_error{"Model metadata is missing identity or feature version"};
    impl_->model_path = model_path;

    std::ifstream schema_input{schema_path};
    std::string schema_contents((std::istreambuf_iterator<char>(schema_input)), std::istreambuf_iterator<char>());
    const auto feature_key = schema_contents.find("\"features\"");
    const auto feature_begin = feature_key == std::string::npos ? std::string::npos : schema_contents.find('[', feature_key);
    const auto feature_end = feature_begin == std::string::npos ? std::string::npos : schema_contents.find(']', feature_begin);
    if (feature_begin != std::string::npos && feature_end != std::string::npos) {
        const std::regex feature_pattern("\\\"([^\\\"]+)\\\"");
        const std::string feature_text = schema_contents.substr(feature_begin, feature_end - feature_begin);
        for (std::sregex_iterator it(feature_text.begin(), feature_text.end(), feature_pattern), end; it != end; ++it) impl_->feature_names.push_back((*it)[1].str());
    }
    if (impl_->feature_names.empty()) throw std::runtime_error{"Feature schema contains no feature names"};
    impl_->booster = std::make_unique<Booster>(model_path);
}

Prediction SectorXGBoostModel::predict(const std::vector<float>& features) const {
    if (features.empty()) {
        throw std::invalid_argument{"Feature vector cannot be empty"};
    }
    const auto row_count = features.size();
    if (row_count != impl_->feature_names.size()) {
        throw std::invalid_argument{"Feature count does not match the loaded model schema"};
    }

    Dataset dataset;
    dataset.feature_names = {"ret_1", "ret_3", "ret_6", "volatility_6", "volume_mean_6", "rel_volume", "rsi_14", "spy_ret_1", "spy_ret_3", "sector_spy_diff"};
    dataset.features = features;
    dataset.labels = {0.0F};
    DMatrix matrix{dataset};

    const auto predictions = impl_->booster->predict(matrix.get());
    if (predictions.empty()) {
        throw std::runtime_error{"XGBoost prediction was empty"};
    }

    return Prediction{
        .model_id = impl_->model_id,
        .symbol = "XLK",
        .event_time = "",
        .predicted_return = static_cast<double>(predictions.front()),
        .prediction_horizon_bars = impl_->prediction_horizon,
        .feature_version = impl_->feature_version,
    };
}

const std::string& SectorXGBoostModel::model_id() const noexcept {
    return impl_->model_id;
}

const std::string& SectorXGBoostModel::feature_version() const noexcept {
    return impl_->feature_version;
}

const std::string& SectorXGBoostModel::target_symbol() const noexcept { return impl_->target_symbol; }
const std::string& SectorXGBoostModel::feature_schema_hash() const noexcept { return impl_->schema_hash; }
const std::vector<std::string>& SectorXGBoostModel::feature_names() const noexcept { return impl_->feature_names; }

}  // namespace arrakis::model
