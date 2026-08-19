#include <xgboost/c_api.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

void check_xgboost(const int result, const std::string_view operation) {
    if (result != 0) {
        throw std::runtime_error{std::string{operation} + " failed: " + XGBGetLastError()};
    }
}

[[nodiscard]] std::vector<std::string> read_record(std::istream& input) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    char character = 0;
    while (input.get(character)) {
        if (character == '"') {
            if (quoted && input.peek() == '"') {
                input.get(character);
                field.push_back(character);
            } else {
                quoted = !quoted;
            }
        } else if (character == ',' && !quoted) {
            fields.push_back(field);
            field.clear();
        } else if ((character == '\n' || character == '\r') && !quoted) {
            if (character == '\r' && input.peek() == '\n') input.get(character);
            fields.push_back(field);
            return fields;
        } else {
            field.push_back(character);
        }
    }
    if (quoted) throw std::runtime_error{"CSV ended inside a quoted field"};
    if (!field.empty() || !fields.empty()) fields.push_back(field);
    return fields;
}

[[nodiscard]] std::size_t column_index(
    const std::vector<std::string>& header,
    const std::string_view name
) {
    const auto found = std::ranges::find(header, name);
    if (found == header.end()) throw std::runtime_error{"Missing CSV column: " + std::string{name}};
    return static_cast<std::size_t>(std::distance(header.begin(), found));
}

struct Dataset final {
    std::vector<std::string> dates;
    std::vector<std::string> feature_names;
    std::vector<float> features;
    std::vector<float> labels;

    [[nodiscard]] std::size_t rows() const noexcept { return dates.size(); }
    [[nodiscard]] std::size_t columns() const noexcept { return feature_names.size(); }
};

[[nodiscard]] Dataset load_dataset(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open ranking dataset: " + path.string()};
    const auto header = read_record(input);
    const auto date_index = column_index(header, "date");
    const auto target_index = column_index(header, "target_next_close_up");

    std::vector<std::size_t> feature_indices;
    Dataset dataset;
    for (std::size_t index = 0; index < header.size(); ++index) {
        // Sector identity is excluded because the ranker must generalize across
        // sectors rather than memorize a static sector prior.
        if (index == date_index || index == target_index || header[index] == "sector_id") continue;
        feature_indices.push_back(index);
        dataset.feature_names.push_back(header[index]);
    }
    if (feature_indices.empty()) throw std::runtime_error{"Ranking dataset has no usable features"};

    while (true) {
        const auto fields = read_record(input);
        if (fields.empty()) break;
        if (fields.size() != header.size()) throw std::runtime_error{"Malformed ranking dataset row"};
        const auto& date = fields[date_index];
        const auto target = std::stof(fields[target_index]);
        if (target != 0.0F && target != 1.0F) throw std::runtime_error{"Ranking labels must be binary"};
        if (!dataset.dates.empty() && date < dataset.dates.back()) {
            throw std::runtime_error{"Ranking dataset must be sorted chronologically"};
        }
        dataset.dates.push_back(date);
        dataset.labels.push_back(target);
        for (const auto index : feature_indices) {
            const auto& value = fields[index];
            const auto parsed = value.empty() || value == "nan"
                                    ? std::numeric_limits<float>::quiet_NaN()
                                    : std::stof(value);
            if (!std::isfinite(parsed)) throw std::runtime_error{"Ranking features must be finite"};
            dataset.features.push_back(parsed);
        }
    }
    if (dataset.rows() == 0) throw std::runtime_error{"Ranking dataset is empty"};
    return dataset;
}

[[nodiscard]] std::string session_key(const std::string_view date_key) {
    const auto separator = date_key.find('|');
    if (separator == std::string_view::npos) throw std::runtime_error{"Ranking row lacks a session key"};
    return std::string{date_key.substr(0, separator)};
}

[[nodiscard]] std::string session_symbol(const std::string_view date_key) {
    const auto separator = date_key.find('|');
    if (separator == std::string_view::npos || separator + 1 >= date_key.size()) {
        throw std::runtime_error{"Ranking row lacks an ETF symbol"};
    }
    return std::string{date_key.substr(separator + 1)};
}

struct Range final {
    std::string key;
    std::size_t begin{};
    std::size_t end{};
};

[[nodiscard]] std::vector<Range> session_ranges(const Dataset& dataset) {
    std::vector<Range> ranges;
    for (std::size_t index = 0; index < dataset.rows(); ++index) {
        const auto key = session_key(dataset.dates[index]);
        if (ranges.empty() || ranges.back().key != key) {
            ranges.push_back(Range{.key = key, .begin = index, .end = index + 1});
        } else {
            ranges.back().end = index + 1;
        }
    }
    return ranges;
}

[[nodiscard]] std::vector<Range> month_ranges(const Dataset& dataset) {
    std::vector<Range> ranges;
    for (std::size_t index = 0; index < dataset.rows(); ++index) {
        const auto key = dataset.dates[index].substr(0, 7);
        if (ranges.empty() || ranges.back().key != key) {
            ranges.push_back(Range{.key = key, .begin = index, .end = index + 1});
        } else {
            ranges.back().end = index + 1;
        }
    }
    return ranges;
}

struct Slice final {
    const Dataset& source;
    std::size_t begin{};
    std::size_t end{};
};

[[nodiscard]] std::vector<unsigned> groups_for_slice(const Slice slice) {
    std::vector<unsigned> groups;
    if (slice.begin == slice.end) return groups;
    auto current = session_key(slice.source.dates[slice.begin]);
    unsigned count = 0;
    for (std::size_t index = slice.begin; index < slice.end; ++index) {
        const auto key = session_key(slice.source.dates[index]);
        if (key != current) {
            groups.push_back(count);
            current = key;
            count = 0;
        }
        ++count;
    }
    groups.push_back(count);
    return groups;
}

void validate_complete_sessions(const Dataset& dataset) {
    for (const auto& range : session_ranges(dataset)) {
        if (range.end - range.begin != 11) {
            throw std::runtime_error{
                "Ranking target requires exactly 11 contiguous sector rows per session: " + range.key
            };
        }
        std::unordered_set<std::string> symbols;
        for (std::size_t row = range.begin; row < range.end; ++row) {
            symbols.insert(session_symbol(dataset.dates[row]));
        }
        if (symbols.size() != 11) {
            throw std::runtime_error{"Ranking session contains duplicate or missing ETF symbols: " + range.key};
        }
    }
}

class Matrix final {
  public:
    explicit Matrix(const Slice slice) : slice_(slice), groups_(groups_for_slice(slice)) {
        if (slice.begin >= slice.end) throw std::invalid_argument{"Cannot create an empty ranking matrix"};
        const auto feature_offset = slice.begin * slice.source.columns();
        check_xgboost(
            XGDMatrixCreateFromMat(
                slice.source.features.data() + static_cast<std::ptrdiff_t>(feature_offset),
                static_cast<bst_ulong>(slice.end - slice.begin),
                static_cast<bst_ulong>(slice.source.columns()),
                std::numeric_limits<float>::quiet_NaN(),
                &handle_
            ),
            "Creating ranking DMatrix"
        );
        check_xgboost(
            XGDMatrixSetFloatInfo(
                handle_, "label", slice.source.labels.data() + static_cast<std::ptrdiff_t>(slice.begin),
                static_cast<bst_ulong>(slice.end - slice.begin)
            ),
            "Setting ranking labels"
        );
        check_xgboost(
            XGDMatrixSetUIntInfo(
                handle_, "group", groups_.data(), static_cast<bst_ulong>(groups_.size())
            ),
            "Setting ranking groups"
        );
    }

    ~Matrix() {
        if (handle_ != nullptr) static_cast<void>(XGDMatrixFree(handle_));
    }
    Matrix(const Matrix&) = delete;
    Matrix& operator=(const Matrix&) = delete;
    [[nodiscard]] DMatrixHandle get() const noexcept { return handle_; }

  private:
    Slice slice_;
    std::vector<unsigned> groups_;
    DMatrixHandle handle_{nullptr};
};

class Booster final {
  public:
    explicit Booster(const std::vector<DMatrixHandle>& matrices) {
        check_xgboost(
            XGBoosterCreate(matrices.data(), static_cast<bst_ulong>(matrices.size()), &handle_),
            "Creating ranker"
        );
    }
    ~Booster() { if (handle_ != nullptr) static_cast<void>(XGBoosterFree(handle_)); }
    Booster(const Booster&) = delete;
    Booster& operator=(const Booster&) = delete;

    void set(const std::string_view name, const std::string_view value) {
        const std::string name_copy{name};
        const std::string value_copy{value};
        check_xgboost(XGBoosterSetParam(handle_, name_copy.c_str(), value_copy.c_str()), "Setting ranker parameter");
    }
    void update(const int iteration, const DMatrixHandle train) {
        check_xgboost(XGBoosterUpdateOneIter(handle_, iteration, train), "Updating ranker");
    }
    [[nodiscard]] std::string evaluate(
        const int iteration, std::vector<DMatrixHandle> matrices,
        std::vector<const char*> names
    ) const {
        const char* result = nullptr;
        check_xgboost(
            XGBoosterEvalOneIter(
                handle_, iteration, matrices.data(), names.data(),
                static_cast<bst_ulong>(matrices.size()), &result
            ),
            "Evaluating ranker"
        );
        return result == nullptr ? std::string{} : std::string{result};
    }
    [[nodiscard]] std::vector<float> predict(const DMatrixHandle matrix) const {
        constexpr std::string_view config =
            R"({"type":0,"training":false,"iteration_begin":0,"iteration_end":0,"strict_shape":true})";
        const bst_ulong* shape = nullptr;
        bst_ulong dimensions = 0;
        const float* predictions = nullptr;
        check_xgboost(
            XGBoosterPredictFromDMatrix(handle_, matrix, config.data(), &shape, &dimensions, &predictions),
            "Predicting ranker"
        );
        if (dimensions == 0 || shape == nullptr || predictions == nullptr) throw std::runtime_error{"Invalid ranker predictions"};
        std::size_t count = 1;
        for (bst_ulong index = 0; index < dimensions; ++index) count *= static_cast<std::size_t>(shape[index]);
        return {predictions, predictions + count};
    }
    void save(const std::filesystem::path& path) const {
        check_xgboost(XGBoosterSaveModel(handle_, path.string().c_str()), "Saving ranker checkpoint");
    }
    void load(const std::filesystem::path& path) {
        check_xgboost(XGBoosterLoadModel(handle_, path.string().c_str()), "Loading ranker checkpoint");
    }

  private:
    BoosterHandle handle_{nullptr};
};

struct Metrics final {
    double auc{};
    double top1_hit_rate{};
    std::size_t groups{};
};

[[nodiscard]] Metrics evaluate_scores(const Slice slice, const std::vector<float>& scores) {
    if (scores.size() != slice.end - slice.begin) throw std::runtime_error{"Ranker score length mismatch"};
    std::vector<std::pair<float, float>> ranked;
    ranked.reserve(scores.size());
    for (std::size_t index = 0; index < scores.size(); ++index) {
        ranked.emplace_back(scores[index], slice.source.labels[slice.begin + index]);
    }
    std::ranges::sort(ranked, {}, &std::pair<float, float>::first);
    double positive_rank_sum = 0.0;
    std::size_t positives = 0;
    std::size_t index = 0;
    while (index < ranked.size()) {
        auto tie_end = index + 1;
        while (tie_end < ranked.size() && ranked[tie_end].first == ranked[index].first) ++tie_end;
        const auto average_rank = (static_cast<double>(index + 1) + static_cast<double>(tie_end)) / 2.0;
        for (auto tie_index = index; tie_index < tie_end; ++tie_index) {
            if (ranked[tie_index].second == 1.0F) {
                positive_rank_sum += average_rank;
                ++positives;
            }
        }
        index = tie_end;
    }
    const auto negatives = ranked.size() - positives;
    if (positives == 0 || negatives == 0) throw std::runtime_error{"Ranker slice lacks both labels"};
    const auto auc = (positive_rank_sum - static_cast<double>(positives) * (static_cast<double>(positives) + 1.0) / 2.0) /
                     (static_cast<double>(positives) * static_cast<double>(negatives));

    std::size_t group_count = 0;
    std::size_t top1_hits = 0;
    std::size_t group_begin = 0;
    while (group_begin < scores.size()) {
        const auto key = session_key(slice.source.dates[slice.begin + group_begin]);
        std::size_t group_end = group_begin + 1;
        while (group_end < scores.size() && session_key(slice.source.dates[slice.begin + group_end]) == key) ++group_end;
        std::size_t best = group_begin;
        for (std::size_t row = group_begin + 1; row < group_end; ++row) {
            if (scores[row] > scores[best]) best = row;
        }
        bool has_positive = false;
        for (std::size_t row = group_begin; row < group_end; ++row) {
            if (slice.source.labels[slice.begin + row] == 1.0F) {
                has_positive = true;
                break;
            }
        }
        if (has_positive) {
            ++group_count;
            if (slice.source.labels[slice.begin + best] == 1.0F) ++top1_hits;
        }
        group_begin = group_end;
    }
    return Metrics{.auc = auc, .top1_hit_rate = group_count == 0 ? 0.0 : static_cast<double>(top1_hits) / static_cast<double>(group_count), .groups = group_count};
}

[[nodiscard]] double validation_auc(const std::string& evaluation) {
    const std::string marker{"validation-auc:"};
    const auto position = evaluation.find(marker);
    if (position == std::string::npos) return -std::numeric_limits<double>::infinity();
    return std::stod(evaluation.substr(position + marker.size()));
}

struct Fold final {
    std::string month;
    Range train;
    Range validation;
    Range test;
    int best_iteration{};
    Metrics validation_metrics{};
    Metrics test_metrics{};
    std::vector<float> test_scores;
};

[[nodiscard]] Fold train_fold(
    const Dataset& dataset, const Range& test_month, const Range& validation_month,
    const std::size_t purge_rows, const int max_rounds, const int patience,
    const std::filesystem::path& checkpoint
) {
    if (validation_month.begin < purge_rows || test_month.begin < purge_rows) throw std::runtime_error{"Ranker purge exceeds fold boundary"};
    const Range train{.key = "train", .begin = 0, .end = validation_month.begin - purge_rows};
    const Range validation{.key = "validation", .begin = validation_month.begin, .end = test_month.begin - purge_rows};
    const Range test{.key = "test", .begin = test_month.begin, .end = test_month.end};
    if (train.end <= train.begin || validation.end <= validation.begin || test.end <= test.begin) throw std::runtime_error{"Ranker fold has an empty slice"};
    Matrix train_matrix{Slice{dataset, train.begin, train.end}};
    Matrix validation_matrix{Slice{dataset, validation.begin, validation.end}};
    Booster booster{{train_matrix.get(), validation_matrix.get()}};
    booster.set("objective", "rank:pairwise");
    booster.set("eval_metric", "auc");
    booster.set("eta", "0.03");
    booster.set("max_depth", "2");
    booster.set("min_child_weight", "10");
    booster.set("subsample", "0.8");
    booster.set("colsample_bytree", "0.8");
    booster.set("lambda", "10.0");
    booster.set("alpha", "0.0");
    booster.set("tree_method", "hist");
    booster.set("seed", "42");
    booster.set("nthread", "1");
    booster.set("base_score", "0.5");
    booster.set("lambdarank_pair_method", "topk");
    booster.set("lambdarank_num_pair_per_sample", "11");
    booster.set("lambdarank_normalization", "true");
    booster.set("lambdarank_score_normalization", "true");
    booster.set("lambdarank_unbiased", "false");
    const std::vector<DMatrixHandle> evaluation_matrices{train_matrix.get(), validation_matrix.get()};
    const std::vector<const char*> evaluation_names{"train", "validation"};
    double best_metric = -std::numeric_limits<double>::infinity();
    int best_iteration = 0;
    int stale = 0;
    for (int iteration = 0; iteration < max_rounds; ++iteration) {
        booster.update(iteration, train_matrix.get());
        const auto evaluation = booster.evaluate(iteration, evaluation_matrices, evaluation_names);
        const auto metric = validation_auc(evaluation);
        if (metric > best_metric + 1.0e-12) {
            best_metric = metric;
            best_iteration = iteration + 1;
            stale = 0;
            booster.save(checkpoint);
        } else if (++stale >= patience) {
            break;
        }
    }
    if (best_iteration == 0) throw std::runtime_error{"Ranker early stopping never produced a checkpoint"};
    booster.load(checkpoint);
    std::error_code error;
    std::filesystem::remove(checkpoint, error);
    Matrix test_matrix{Slice{dataset, test.begin, test.end}};
    const auto validation_predictions = booster.predict(validation_matrix.get());
    const auto test_predictions = booster.predict(test_matrix.get());
    return Fold{
        .month = test_month.key,
        .train = train,
        .validation = validation,
        .test = test,
        .best_iteration = best_iteration,
        .validation_metrics = evaluate_scores(Slice{dataset, validation.begin, validation.end}, validation_predictions),
        .test_metrics = evaluate_scores(Slice{dataset, test.begin, test.end}, test_predictions),
        .test_scores = test_predictions,
    };
}

void write_results(
    const std::filesystem::path& output, const Dataset& dataset, const std::vector<Fold>& folds,
    const std::vector<std::string>& prediction_dates, const std::vector<float>& labels,
    const std::vector<float>& scores, const std::size_t purge_rows
) {
    auto predictions_path = output;
    predictions_path += ".oos_predictions.csv";
    std::ofstream predictions{predictions_path};
    if (!predictions) throw std::runtime_error{"Could not write ranker predictions"};
    predictions << "date,label,rank_score\n";
    for (std::size_t index = 0; index < scores.size(); ++index) predictions << prediction_dates[index] << ',' << labels[index] << ',' << scores[index] << '\n';

    if (scores.empty() || scores.size() != labels.size() || scores.size() != prediction_dates.size()) {
        throw std::runtime_error{"Ranker OOS predictions are incomplete"};
    }
    double positive_rank_sum = 0.0;
    std::vector<std::pair<float, float>> ranked;
    ranked.reserve(scores.size());
    for (std::size_t index = 0; index < scores.size(); ++index) ranked.emplace_back(scores[index], labels[index]);
    std::ranges::sort(ranked, {}, &std::pair<float, float>::first);
    std::size_t positives = 0;
    for (std::size_t index = 0; index < ranked.size();) {
        auto end = index + 1;
        while (end < ranked.size() && ranked[end].first == ranked[index].first) ++end;
        const auto rank = (static_cast<double>(index + 1) + static_cast<double>(end)) / 2.0;
        for (std::size_t row = index; row < end; ++row) if (ranked[row].second == 1.0F) { positive_rank_sum += rank; ++positives; }
        index = end;
    }
    const auto negatives = ranked.size() - positives;
    const auto pooled_auc = (positive_rank_sum - static_cast<double>(positives) * (static_cast<double>(positives) + 1.0) / 2.0) /
                            (static_cast<double>(positives) * static_cast<double>(negatives));

    std::ofstream result{output};
    if (!result) throw std::runtime_error{"Could not write ranker report"};
    result << std::fixed << std::setprecision(6)
           << "{\n  \"protocol\": {\n"
           << "    \"dataset\": \"" << dataset.rows() << " rows, " << dataset.columns() << " features\",\n"
           << "    \"objective\": \"rank:pairwise\",\n"
           << "    \"group_key\": \"session date before | sector\",\n"
           << "    \"feature_policy\": \"all numeric features except sector_id\",\n"
           << "    \"eta\": 0.03, \"max_depth\": 2, \"min_child_weight\": 10, \"lambda\": 10,\n"
           << "    \"subsample\": 0.8, \"colsample_bytree\": 0.8, \"max_rounds\": 400, \"early_stopping_rounds\": 30,\n"
           << "    \"purge_rows\": " << purge_rows << ",\n"
           << "    \"promotion_gate\": \"validation and test AUC > 0.55 in every monthly fold\"\n  },\n  \"windows\": [\n";
    for (std::size_t index = 0; index < folds.size(); ++index) {
        const auto& fold = folds[index];
        result << "    {\"test_month\": \"" << fold.month << "\", \"best_iteration\": " << fold.best_iteration
               << ", \"train_rows\": " << (fold.train.end - fold.train.begin)
               << ", \"validation_rows\": " << (fold.validation.end - fold.validation.begin)
               << ", \"test_rows\": " << (fold.test.end - fold.test.begin)
               << ", \"validation_auc\": " << fold.validation_metrics.auc
               << ", \"validation_top1_hit_rate\": " << fold.validation_metrics.top1_hit_rate
               << ", \"test_auc\": " << fold.test_metrics.auc
               << ", \"test_top1_hit_rate\": " << fold.test_metrics.top1_hit_rate << "}"
               << (index + 1 == folds.size() ? "\n" : ",\n");
    }
    result << "  ],\n  \"pooled_test_auc\": " << pooled_auc << ",\n  \"folds\": " << folds.size() << "\n}\n";
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 5) {
            std::cout << "Usage: arrakis-train-xgboost-ranker <dataset.csv> <output.json> <purge-rows> <checkpoint-dir>\n";
            return 0;
        }
        const auto dataset = load_dataset(argv[1]);
        validate_complete_sessions(dataset);
        const auto purge_rows = static_cast<std::size_t>(std::stoull(argv[3]));
        const auto checkpoint_dir = std::filesystem::path{argv[4]};
        std::filesystem::create_directories(checkpoint_dir);
        const auto months = month_ranges(dataset);
        if (months.size() < 12) throw std::runtime_error{"Ranker requires at least 12 calendar months"};
        const auto first_year = std::stoi(months.front().key.substr(0, 4));
        std::vector<Fold> folds;
        std::vector<std::string> prediction_dates;
        std::vector<float> prediction_labels;
        std::vector<float> prediction_scores;
        for (std::size_t index = 0; index < months.size(); ++index) {
            if (std::stoi(months[index].key.substr(0, 4)) < first_year + 2 || index < 6) continue;
            const auto& validation_month = months[index - 6];
            const auto& test_month = months[index];
            const auto checkpoint = checkpoint_dir / ("ranker_" + test_month.key + ".json");
            auto fold = train_fold(dataset, test_month, validation_month, purge_rows, 400, 30, checkpoint);
            for (std::size_t row = fold.test.begin; row < fold.test.end; ++row) {
                prediction_dates.push_back(dataset.dates[row]);
                prediction_labels.push_back(dataset.labels[row]);
                prediction_scores.push_back(fold.test_scores[row - fold.test.begin]);
            }
            folds.push_back(fold);
        }
        write_results(argv[2], dataset, folds, prediction_dates, prediction_labels, prediction_scores, purge_rows);
        std::cout << "Ranker monthly walk-forward: " << folds.size() << " folds\n";
        for (const auto& fold : folds) std::cout << fold.month << " validation_auc=" << fold.validation_metrics.auc << " test_auc=" << fold.test_metrics.auc << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-train-xgboost-ranker: " << error.what() << '\n';
        return 1;
    }
}
