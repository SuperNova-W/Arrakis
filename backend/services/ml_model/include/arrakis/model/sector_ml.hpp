#pragma once

#include "arrakis/model/dataset.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace arrakis::model {

struct BarRecord final {
    std::string timestamp;
    std::string symbol;
    double open{0.0};
    double high{0.0};
    double low{0.0};
    double close{0.0};
    double volume{0.0};
};

struct FeatureConfiguration final {
    std::string target_symbol{"XLK"};
    std::vector<std::string> context_symbols{"SPY", "QQQ", "IWM", "TLT", "HYG", "GLD", "USO"};
    int bar_interval_minutes{5};
    int prediction_horizon_bars{6};
    int lookback_window{12};
    std::string feature_version{"sector-features-v1"};
    std::string training_start{};
    std::string validation_start{};
    std::string test_start{};
    int max_rounds{25};
    int early_stopping_rounds{10};
    int random_seed{1337};
    double validation_fraction{0.2};
    int purge_bars{1};
    int embargo_bars{0};
    std::string objective{"reg:squarederror"};
    std::string eval_metric{"rmse"};
    int max_depth{4};
    double eta{0.1};
    double subsample{0.9};
    double colsample_bytree{0.8};
    double gamma{0.0};
    double lambda{1.0};
    double alpha{0.0};
    int nthread{1};
    std::string tree_method{"hist"};
};

struct Prediction final {
    std::string model_id;
    std::string symbol;
    std::string event_time;
    double predicted_return{0.0};
    int prediction_horizon_bars{0};
    std::string feature_version;
};

class SectorFeatureBuilder final {
  public:
    explicit SectorFeatureBuilder(FeatureConfiguration config);

    [[nodiscard]] Dataset build(const std::vector<BarRecord>& bars) const;

    [[nodiscard]] DatasetSplit build_split(
        const Dataset& dataset,
        double validation_fraction,
        int purge_bars,
        int embargo_bars
    ) const;

    [[nodiscard]] const std::vector<std::string>& feature_names() const noexcept;

  private:
    FeatureConfiguration config_;
    mutable std::vector<std::string> feature_names_;
};

class SectorXGBoostTrainer final {
  public:
    explicit SectorXGBoostTrainer(FeatureConfiguration config);

    [[nodiscard]] std::filesystem::path train(
        const Dataset& training_data,
        const std::filesystem::path& output_dir
    ) const;

  private:
    FeatureConfiguration config_;
};

class SectorXGBoostModel final {
  public:
    SectorXGBoostModel();
    ~SectorXGBoostModel();

    SectorXGBoostModel(const SectorXGBoostModel&) = delete;
    SectorXGBoostModel& operator=(const SectorXGBoostModel&) = delete;
    SectorXGBoostModel(SectorXGBoostModel&&) noexcept;
    SectorXGBoostModel& operator=(SectorXGBoostModel&&) noexcept;

    void load(
        const std::filesystem::path& model_path,
        const std::filesystem::path& metadata_path,
        const std::filesystem::path& schema_path
    );

    [[nodiscard]] Prediction predict(const std::vector<float>& features) const;

    [[nodiscard]] const std::string& model_id() const noexcept;
    [[nodiscard]] const std::string& feature_version() const noexcept;
    [[nodiscard]] const std::string& target_symbol() const noexcept;
    [[nodiscard]] const std::string& feature_schema_hash() const noexcept;
    [[nodiscard]] const std::vector<std::string>& feature_names() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace arrakis::model
