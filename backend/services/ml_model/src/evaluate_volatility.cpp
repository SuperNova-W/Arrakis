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
#include <random>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct Prediction final {
    std::string date;
    float label{};
    float raw_probability{};
    float calibrated_probability{};
};

struct MarketHistory final {
    std::vector<std::string> dates;
    std::vector<double> opens;
    std::vector<double> closes;
    std::unordered_map<std::string, std::size_t> index_by_date;
};

struct Sample final {
    std::string date;
    std::string month;
    std::size_t market_index{};
    float label{};
    double rolling_volatility{};
    double ewma_volatility{};
    double previous_abs_open_to_close{};
};

struct Options final {
    std::filesystem::path predictions;
    std::filesystem::path dataset;
    std::filesystem::path market_data;
    std::filesystem::path output;
    std::filesystem::path threshold_predictions;
    std::size_t bootstrap_samples{1000};
    std::size_t block_length{20};
    unsigned int seed{20260810U};
};

struct ClassificationMetrics final {
    double auc{};
    double log_loss{};
    double brier{};
    double balanced_accuracy{};
    double ece{};
    double calibration_slope{};
};

struct LogisticMap final {
    double intercept{};
    double slope{1.0};
    double mean{};
    double scale{1.0};

    [[nodiscard]] double apply(const double score) const {
        const auto standardized = (score - mean) / scale;
        const auto linear = std::clamp(intercept + slope * standardized, -30.0, 30.0);
        return 1.0 / (1.0 + std::exp(-linear));
    }
};

struct StrategyStats final {
    double average_exposure{};
    double average_daily_return{};
    double sharpe{};
    double cagr{};
    double max_drawdown{};
    double turnover{};
    double final_equity{};
};

struct StrategySet final {
    std::map<int, StrategyStats> model;
    std::map<int, StrategyStats> matched_buy_and_hold;
    std::map<int, StrategyStats> ewma;
    std::map<int, StrategyStats> top_quartile_overlay;
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
    if (gmtime_r(&seconds, &utc) == nullptr) {
        throw std::runtime_error{"Could not convert market timestamp to date"};
    }
    std::array<char, 11> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d", &utc) == 0) {
        throw std::runtime_error{"Could not format market timestamp date"};
    }
    return buffer.data();
}

[[nodiscard]] MarketHistory load_market_history(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open market history: " + path.string()};
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error{"Market history is empty"};

    MarketHistory history;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split_csv_line(line);
        if (fields.size() < 6) throw std::runtime_error{"Market row has too few fields"};
        const auto date = date_from_epoch(std::stoll(fields[1]));
        if (!history.dates.empty() && date <= history.dates.back()) {
            throw std::runtime_error{"Market dates must be strictly increasing"};
        }
        history.index_by_date.emplace(date, history.dates.size());
        history.dates.push_back(date);
        history.opens.push_back(std::stod(fields[2]));
        history.closes.push_back(std::stod(fields[5]));
    }
    if (history.dates.empty()) throw std::runtime_error{"Market history has no rows"};
    return history;
}

[[nodiscard]] std::vector<std::string> load_dataset_dates(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open feature dataset: " + path.string()};
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error{"Feature dataset is empty"};
    std::vector<std::string> dates;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split_csv_line(line);
        if (fields.empty() || fields[0].empty()) throw std::runtime_error{"Dataset row has no date"};
        dates.push_back(fields[0]);
    }
    if (dates.empty()) throw std::runtime_error{"Feature dataset has no rows"};
    return dates;
}

[[nodiscard]] std::vector<Prediction> load_predictions(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open prediction file: " + path.string()};
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error{"Prediction file is empty"};
    const auto header = split_csv_line(line);
    const auto calibrated_column = std::ranges::find(header, "calibrated_probability_positive_label");
    const auto raw_column = std::ranges::find(header, "probability_positive_label");
    if (raw_column == header.end() || calibrated_column == header.end()) {
        throw std::invalid_argument{"Prediction file must contain raw and calibrated probabilities"};
    }
    const auto raw_index = static_cast<std::size_t>(std::distance(header.begin(), raw_column));
    const auto calibrated_index = static_cast<std::size_t>(
        std::distance(header.begin(), calibrated_column)
    );

    std::vector<Prediction> predictions;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split_csv_line(line);
        if (fields.size() <= std::max(raw_index, calibrated_index) || fields.size() < 3) {
            throw std::runtime_error{"Prediction row has too few fields"};
        }
        predictions.push_back(Prediction{
            .date = fields[1],
            .label = std::stof(fields[2]),
            .raw_probability = std::stof(fields[raw_index]),
            .calibrated_probability = std::stof(fields[calibrated_index]),
        });
    }
    if (predictions.empty()) throw std::runtime_error{"Prediction file has no rows"};
    return predictions;
}

[[nodiscard]] double mean_window(
    const std::vector<double>& values,
    const std::size_t begin,
    const std::size_t end
) {
    double sum = 0.0;
    for (std::size_t index = begin; index < end; ++index) sum += values[index];
    return sum / static_cast<double>(end - begin);
}

[[nodiscard]] double standard_deviation_window(
    const std::vector<double>& values,
    const std::size_t begin,
    const std::size_t end
) {
    const auto mean = mean_window(values, begin, end);
    double squared_error = 0.0;
    for (std::size_t index = begin; index < end; ++index) {
        const auto error = values[index] - mean;
        squared_error += error * error;
    }
    return std::sqrt(squared_error / static_cast<double>(end - begin));
}

[[nodiscard]] double rolling_volatility(
    const MarketHistory& history,
    const std::size_t index,
    const std::size_t session_count
) {
    if (index < session_count) throw std::invalid_argument{"Insufficient rolling history"};
    std::vector<double> returns;
    returns.reserve(session_count);
    const auto begin = index - session_count + 1;
    for (std::size_t current = begin; current <= index; ++current) {
        returns.push_back(std::log(history.closes[current] / history.closes[current - 1]));
    }
    return standard_deviation_window(returns, 0, returns.size());
}

[[nodiscard]] std::vector<Sample> build_samples(
    const std::vector<std::string>& dataset_dates,
    const MarketHistory& market
) {
    std::vector<double> intraday_returns(market.dates.size(), 0.0);
    constexpr double alpha = 2.0 / 21.0;
    std::vector<double> ewma_variances(market.dates.size(), 0.0);
    for (std::size_t index = 1; index < market.dates.size(); ++index) {
        intraday_returns[index] =
            std::log(market.closes[index] / market.opens[index]);
        ewma_variances[index] = (1.0 - alpha) * ewma_variances[index - 1] +
                                alpha * intraday_returns[index] * intraday_returns[index];
    }

    std::vector<Sample> samples;
    for (const auto& date : dataset_dates) {
        const auto market_it = market.index_by_date.find(date);
        if (market_it == market.index_by_date.end()) continue;
        const auto index = market_it->second;
        if (index < 60 || index + 1 >= market.dates.size()) continue;
        constexpr std::size_t threshold_window = 20;
        std::array<double, threshold_window> absolute_returns{};
        const auto threshold_begin = index - threshold_window + 1;
        for (std::size_t offset = 0; offset < threshold_window; ++offset) {
            const auto current = threshold_begin + offset;
            absolute_returns[offset] =
                std::abs(market.closes[current] / market.opens[current] - 1.0);
        }
        std::ranges::sort(absolute_returns);
        const auto threshold =
            (absolute_returns[threshold_window / 2 - 1] + absolute_returns[threshold_window / 2]) /
            2.0;
        const auto next_return = market.closes[index + 1] / market.opens[index + 1] - 1.0;
        samples.push_back(Sample{
            .date = date,
            .month = date.substr(0, 7),
            .market_index = index,
            .label = static_cast<float>(std::abs(next_return) > threshold ? 1.0 : 0.0),
            .rolling_volatility = rolling_volatility(market, index, 20),
            .ewma_volatility = std::sqrt(ewma_variances[index]),
            .previous_abs_open_to_close =
                std::abs(market.closes[index] / market.opens[index] - 1.0),
        });
    }
    if (samples.size() < 100) throw std::runtime_error{"Too few aligned volatility samples"};
    return samples;
}

[[nodiscard]] double auc_from_scores(
    const std::vector<float>& labels,
    const std::vector<double>& scores
) {
    if (labels.size() != scores.size() || labels.empty()) {
        throw std::invalid_argument{"AUC inputs are misaligned"};
    }
    std::vector<std::pair<double, float>> ranked;
    ranked.reserve(labels.size());
    for (std::size_t index = 0; index < labels.size(); ++index) {
        ranked.emplace_back(scores[index], labels[index]);
    }
    std::ranges::sort(ranked, {}, &std::pair<double, float>::first);
    double positive_rank_sum = 0.0;
    std::size_t positive_count = 0;
    std::size_t index = 0;
    while (index < ranked.size()) {
        auto tie_end = index + 1;
        while (tie_end < ranked.size() && ranked[tie_end].first == ranked[index].first) ++tie_end;
        const auto average_rank =
            (static_cast<double>(index + 1) + static_cast<double>(tie_end)) / 2.0;
        for (std::size_t tie_index = index; tie_index < tie_end; ++tie_index) {
            if (ranked[tie_index].second == 1.0F) {
                positive_rank_sum += average_rank;
                ++positive_count;
            }
        }
        index = tie_end;
    }
    const auto negative_count = labels.size() - positive_count;
    if (positive_count == 0 || negative_count == 0) return 0.5;
    const auto positives = static_cast<double>(positive_count);
    const auto negatives = static_cast<double>(negative_count);
    return (positive_rank_sum - positives * (positives + 1.0) / 2.0) /
           (positives * negatives);
}

[[nodiscard]] LogisticMap fit_logistic_map(
    const std::vector<float>& labels,
    const std::vector<double>& scores
) {
    if (labels.size() != scores.size() || labels.empty()) {
        throw std::invalid_argument{"Logistic-map inputs are misaligned"};
    }
    const auto sample_count = static_cast<double>(scores.size());
    const auto positive_count = static_cast<double>(std::ranges::count(labels, 1.0F));
    const auto prior = (positive_count + 0.5) / (sample_count + 1.0);
    LogisticMap result{
        .intercept = std::log(prior / (1.0 - prior)),
        .slope = 1.0,
    };
    result.mean = mean_window(scores, 0, scores.size());
    result.scale = standard_deviation_window(scores, 0, scores.size());
    if (!(result.scale > 1.0e-12)) {
        result.scale = 1.0;
        result.slope = 0.0;
        return result;
    }
    if (positive_count == 0.0 || positive_count == sample_count) {
        result.slope = 0.0;
        return result;
    }

    for (int iteration = 0; iteration < 30; ++iteration) {
        double gradient_intercept = 0.0;
        double gradient_slope = 0.0;
        double h00 = 1.0e-6;
        double h01 = 0.0;
        double h11 = 1.0e-6;
        for (std::size_t index = 0; index < scores.size(); ++index) {
            const auto standardized = (scores[index] - result.mean) / result.scale;
            const auto linear = std::clamp(
                result.intercept + result.slope * standardized, -30.0, 30.0
            );
            const auto probability = 1.0 / (1.0 + std::exp(-linear));
            const auto residual = probability - static_cast<double>(labels[index]);
            const auto curvature = std::max(probability * (1.0 - probability), 1.0e-8);
            gradient_intercept += residual;
            gradient_slope += residual * standardized;
            h00 += curvature;
            h01 += curvature * standardized;
            h11 += curvature * standardized * standardized;
        }
        const auto determinant = h00 * h11 - h01 * h01;
        if (!(determinant > 1.0e-12)) break;
        const auto delta_intercept = (h11 * gradient_intercept - h01 * gradient_slope) / determinant;
        const auto delta_slope = (-h01 * gradient_intercept + h00 * gradient_slope) / determinant;
        result.intercept = std::clamp(result.intercept - delta_intercept, -20.0, 20.0);
        result.slope = std::clamp(result.slope - delta_slope, -20.0, 20.0);
        if (std::abs(delta_intercept) < 1.0e-7 && std::abs(delta_slope) < 1.0e-7) break;
    }
    return result;
}

[[nodiscard]] double log_loss(const std::vector<float>& labels, const std::vector<double>& probabilities) {
    constexpr double epsilon = 1.0e-7;
    double total = 0.0;
    for (std::size_t index = 0; index < labels.size(); ++index) {
        const auto probability = std::clamp(probabilities[index], epsilon, 1.0 - epsilon);
        const auto label = static_cast<double>(labels[index]);
        total -= label * std::log(probability) + (1.0 - label) * std::log(1.0 - probability);
    }
    return total / static_cast<double>(labels.size());
}

[[nodiscard]] ClassificationMetrics calculate_metrics(
    const std::vector<float>& labels,
    const std::vector<double>& probabilities
) {
    if (labels.size() != probabilities.size() || labels.empty()) {
        throw std::invalid_argument{"Metric inputs are misaligned"};
    }
    std::size_t positives = 0;
    std::size_t true_positive = 0;
    std::size_t true_negative = 0;
    double brier = 0.0;
    std::array<std::size_t, 10> bin_counts{};
    std::array<double, 10> bin_labels{};
    std::array<double, 10> bin_probabilities{};
    for (std::size_t index = 0; index < labels.size(); ++index) {
        const auto label = static_cast<double>(labels[index]);
        const auto probability = std::clamp(probabilities[index], 0.0, 1.0);
        if (label == 1.0) ++positives;
        if ((probability >= 0.5) == (label == 1.0)) {
            if (label == 1.0) ++true_positive;
            else ++true_negative;
        }
        const auto error = probability - label;
        brier += error * error;
        const auto bucket = std::min(
            static_cast<std::size_t>(probability * 10.0), static_cast<std::size_t>(9)
        );
        ++bin_counts[bucket];
        bin_labels[bucket] += label;
        bin_probabilities[bucket] += probability;
    }
    const auto negatives = labels.size() - positives;
    double ece = 0.0;
    for (std::size_t bucket = 0; bucket < bin_counts.size(); ++bucket) {
        if (bin_counts[bucket] == 0) continue;
        const auto count = static_cast<double>(bin_counts[bucket]);
        ece += count / static_cast<double>(labels.size()) *
               std::abs(bin_labels[bucket] / count - bin_probabilities[bucket] / count);
    }
    std::vector<double> logits;
    logits.reserve(probabilities.size());
    for (const auto probability : probabilities) {
        const auto bounded = std::clamp(probability, 1.0e-6, 1.0 - 1.0e-6);
        logits.push_back(std::log(bounded / (1.0 - bounded)));
    }
    const auto calibration = fit_logistic_map(labels, logits);
    return ClassificationMetrics{
        .auc = auc_from_scores(labels, probabilities),
        .log_loss = log_loss(labels, probabilities),
        .brier = brier / static_cast<double>(labels.size()),
        .balanced_accuracy = negatives == 0 || positives == 0
                                 ? 0.5
                                 : 0.5 * (static_cast<double>(true_positive) /
                                          static_cast<double>(positives) +
                                          static_cast<double>(true_negative) /
                                          static_cast<double>(negatives)),
        .ece = ece,
        .calibration_slope = calibration.slope,
    };
}

[[nodiscard]] double bootstrap_auc_lower_bound(
    const std::vector<float>& labels,
    const std::vector<double>& scores,
    const std::size_t block_length,
    const std::size_t bootstrap_samples,
    const unsigned int seed
) {
    if (labels.size() != scores.size() || labels.empty()) {
        throw std::invalid_argument{"Bootstrap inputs are misaligned"};
    }
    std::mt19937 generator{seed};
    std::uniform_int_distribution<std::size_t> block_start(0, labels.size() - 1);
    std::vector<double> aucs;
    aucs.reserve(bootstrap_samples);
    for (std::size_t sample = 0; sample < bootstrap_samples; ++sample) {
        std::vector<float> sampled_labels;
        std::vector<double> sampled_scores;
        sampled_labels.reserve(labels.size());
        sampled_scores.reserve(scores.size());
        while (sampled_labels.size() < labels.size()) {
            const auto start = block_start(generator);
            for (std::size_t offset = 0; offset < block_length && sampled_labels.size() < labels.size(); ++offset) {
                const auto index = (start + offset) % labels.size();
                sampled_labels.push_back(labels[index]);
                sampled_scores.push_back(scores[index]);
            }
        }
        const auto positive_count = std::ranges::count(sampled_labels, 1.0F);
        if (positive_count == 0 ||
            static_cast<std::size_t>(positive_count) == sampled_labels.size()) continue;
        aucs.push_back(auc_from_scores(sampled_labels, sampled_scores));
    }
    if (aucs.empty()) return 0.5;
    std::ranges::sort(aucs);
    const auto lower_index = static_cast<std::size_t>(
        std::floor(0.025 * static_cast<double>(aucs.size() - 1))
    );
    return aucs[lower_index];
}

[[nodiscard]] std::vector<double> make_oos_scores(
    const std::vector<Prediction>& predictions,
    const std::vector<Sample>& samples,
    const bool use_rolling
) {
    std::unordered_map<std::string, const Sample*> by_date;
    by_date.reserve(samples.size());
    for (const auto& sample : samples) by_date.emplace(sample.date, &sample);
    std::vector<double> scores;
    scores.reserve(predictions.size());
    for (const auto& prediction : predictions) {
        const auto found = by_date.find(prediction.date);
        if (found == by_date.end()) throw std::runtime_error{"Prediction date is not in dataset"};
        scores.push_back(use_rolling ? found->second->rolling_volatility : found->second->ewma_volatility);
    }
    return scores;
}

[[nodiscard]] std::vector<double> make_previous_abs_scores(
    const std::vector<Prediction>& predictions,
    const std::vector<Sample>& samples
) {
    std::unordered_map<std::string, const Sample*> by_date;
    by_date.reserve(samples.size());
    for (const auto& sample : samples) by_date.emplace(sample.date, &sample);
    std::vector<double> scores;
    scores.reserve(predictions.size());
    for (const auto& prediction : predictions) {
        const auto found = by_date.find(prediction.date);
        if (found == by_date.end()) throw std::runtime_error{"Prediction date is not in dataset"};
        scores.push_back(found->second->previous_abs_open_to_close);
    }
    return scores;
}

[[nodiscard]] std::vector<double> make_next_abs_returns(
    const std::vector<Prediction>& predictions,
    const std::vector<Sample>& samples,
    const MarketHistory& market
) {
    std::unordered_map<std::string, const Sample*> by_date;
    by_date.reserve(samples.size());
    for (const auto& sample : samples) by_date.emplace(sample.date, &sample);
    std::vector<double> returns;
    returns.reserve(predictions.size());
    for (const auto& prediction : predictions) {
        const auto sample = by_date.at(prediction.date);
        const auto next_index = sample->market_index + 1;
        returns.push_back(std::abs(market.closes[next_index] / market.opens[next_index] - 1.0));
    }
    return returns;
}

[[nodiscard]] double empirical_quantile(std::vector<double> values, const double quantile) {
    if (values.empty() || !(quantile >= 0.0 && quantile <= 1.0)) {
        throw std::invalid_argument{"Invalid empirical quantile request"};
    }
    const auto position = static_cast<std::size_t>(
        std::floor(quantile * static_cast<double>(values.size() - 1))
    );
    std::ranges::nth_element(values, values.begin() + static_cast<std::ptrdiff_t>(position));
    return values[position];
}

[[nodiscard]] std::vector<double> make_fold_local_probabilities(
    const std::vector<Prediction>& predictions,
    const std::vector<Sample>& samples,
    const std::vector<double>& scores
) {
    std::map<std::string, std::pair<std::size_t, std::size_t>> month_ranges;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        auto& range = month_ranges[samples[index].month];
        if (range.second == 0) range.first = index;
        range.second = index + 1;
    }
    std::vector<std::string> months;
    months.reserve(month_ranges.size());
    for (const auto& [month, range] : month_ranges) {
        (void)range;
        months.push_back(month);
    }
    std::unordered_map<std::string, std::size_t> month_index;
    for (std::size_t index = 0; index < months.size(); ++index) month_index.emplace(months[index], index);

    std::unordered_map<std::string, std::size_t> sample_index;
    sample_index.reserve(samples.size());
    for (std::size_t index = 0; index < samples.size(); ++index) sample_index.emplace(samples[index].date, index);

    std::vector<double> probabilities;
    probabilities.reserve(predictions.size());
    for (const auto& prediction : predictions) {
        const auto sample_it = sample_index.find(prediction.date);
        if (sample_it == sample_index.end()) throw std::runtime_error{"Missing sample for prediction"};
        const auto sample_position = sample_it->second;
        const auto month_it = month_index.find(samples[sample_position].month);
        if (month_it == month_index.end() || month_it->second < 6) {
            throw std::runtime_error{"Prediction does not have six prior validation months"};
        }
        const auto validation_month = months[month_it->second - 6];
        const auto validation_range = month_ranges.at(validation_month);
        const auto train_end = validation_range.first > 1 ? validation_range.first - 1 : 1;
        std::vector<float> train_labels;
        std::vector<double> train_scores;
        train_labels.reserve(train_end);
        train_scores.reserve(train_end);
        for (std::size_t index = 0; index < train_end; ++index) {
            train_labels.push_back(samples[index].label);
            train_scores.push_back(scores[index]);
        }
        probabilities.push_back(fit_logistic_map(train_labels, train_scores).apply(scores[sample_position]));
    }
    return probabilities;
}

[[nodiscard]] double clamp_exposure(const double exposure) {
    return std::clamp(exposure, 0.25, 1.0);
}

[[nodiscard]] StrategyStats calculate_strategy(
    const std::vector<Prediction>& predictions,
    const std::vector<Sample>& samples,
    const MarketHistory& market,
    const std::vector<double>& exposures,
    const int cost_bps
) {
    if (predictions.size() != exposures.size()) throw std::invalid_argument{"Exposure inputs are misaligned"};
    std::unordered_map<std::string, const Sample*> by_date;
    by_date.reserve(samples.size());
    for (const auto& sample : samples) by_date.emplace(sample.date, &sample);
    std::vector<double> returns;
    returns.reserve(predictions.size());
    double equity = 1.0;
    double peak = 1.0;
    double maximum_drawdown = 0.0;
    double turnover = 0.0;
    double previous_exposure = 0.0;
    double exposure_sum = 0.0;
    const auto cost = static_cast<double>(cost_bps) / 10000.0;
    for (std::size_t index = 0; index < predictions.size(); ++index) {
        const auto sample = by_date.at(predictions[index].date);
        const auto next_index = sample->market_index + 1;
        const auto next_return = market.closes[next_index] / market.opens[next_index] - 1.0;
        const auto change = std::abs(exposures[index] - previous_exposure);
        const auto net_return = exposures[index] * next_return - cost * change;
        returns.push_back(net_return);
        equity *= 1.0 + net_return;
        peak = std::max(peak, equity);
        maximum_drawdown = std::min(maximum_drawdown, equity / peak - 1.0);
        turnover += change;
        exposure_sum += exposures[index];
        previous_exposure = exposures[index];
    }
    const auto count = static_cast<double>(returns.size());
    const auto average_return = std::accumulate(returns.begin(), returns.end(), 0.0) / count;
    double squared_error = 0.0;
    for (const auto value : returns) squared_error += (value - average_return) * (value - average_return);
    const auto standard_deviation = returns.size() > 1
                                        ? std::sqrt(squared_error / static_cast<double>(returns.size() - 1))
                                        : 0.0;
    return StrategyStats{
        .average_exposure = exposure_sum / count,
        .average_daily_return = average_return,
        .sharpe = standard_deviation > 1.0e-12 ? std::sqrt(252.0) * average_return / standard_deviation : 0.0,
        .cagr = equity > 0.0 ? std::pow(equity, 252.0 / count) - 1.0 : -1.0,
        .max_drawdown = maximum_drawdown,
        .turnover = turnover / count,
        .final_equity = equity,
    };
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
        if (argument == "--predictions") options.predictions = require_value();
        else if (argument == "--dataset") options.dataset = require_value();
        else if (argument == "--market-data") options.market_data = require_value();
        else if (argument == "--output") options.output = require_value();
        else if (argument == "--threshold-predictions") options.threshold_predictions = require_value();
        else if (argument == "--bootstrap-samples") options.bootstrap_samples = std::stoul(std::string{require_value()});
        else if (argument == "--block-length") options.block_length = std::stoul(std::string{require_value()});
        else if (argument == "--seed") options.seed = static_cast<unsigned int>(std::stoul(std::string{require_value()}));
        else if (argument == "--help") {
            std::cout << "Usage: arrakis-evaluate-volatility --predictions <csv> --dataset <csv> "
                         "--market-data <csv> --output <json> [--threshold-predictions <dev.csv>]\n";
            std::exit(0);
        } else throw std::invalid_argument{"Unknown argument: " + std::string{argument}};
    }
    if (options.predictions.empty() || options.dataset.empty() || options.market_data.empty() || options.output.empty()) {
        throw std::invalid_argument{"--predictions, --dataset, --market-data, and --output are required"};
    }
    if (options.bootstrap_samples == 0 || options.block_length == 0) {
        throw std::invalid_argument{"Bootstrap samples and block length must be positive"};
    }
    return options;
}

void write_metrics(std::ostream& output, const ClassificationMetrics& metrics) {
    output << "{\"auc\": " << metrics.auc << ", \"log_loss\": " << metrics.log_loss
           << ", \"brier\": " << metrics.brier
           << ", \"balanced_accuracy\": " << metrics.balanced_accuracy
           << ", \"ece\": " << metrics.ece
           << ", \"calibration_slope\": " << metrics.calibration_slope << "}";
}

void write_strategy(std::ostream& output, const std::map<int, StrategyStats>& strategies) {
    output << "{";
    std::size_t index = 0;
    for (const auto& [cost, stats] : strategies) {
        if (index++ > 0) output << ",";
        output << "\"" << cost << "bps\": {\"average_exposure\": " << stats.average_exposure
               << ", \"average_daily_return\": " << stats.average_daily_return
               << ", \"sharpe\": " << stats.sharpe << ", \"cagr\": " << stats.cagr
               << ", \"max_drawdown\": " << stats.max_drawdown
               << ", \"turnover\": " << stats.turnover
               << ", \"final_equity\": " << stats.final_equity << "}";
    }
    output << "}";
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto predictions = load_predictions(options.predictions);
        const auto market = load_market_history(options.market_data);
        const auto dataset_dates = load_dataset_dates(options.dataset);
        const auto samples = build_samples(dataset_dates, market);

        std::vector<float> labels;
        std::vector<double> raw_probabilities;
        std::vector<double> calibrated_probabilities;
        labels.reserve(predictions.size());
        raw_probabilities.reserve(predictions.size());
        calibrated_probabilities.reserve(predictions.size());
        for (const auto& prediction : predictions) {
            labels.push_back(prediction.label);
            raw_probabilities.push_back(prediction.raw_probability);
            calibrated_probabilities.push_back(prediction.calibrated_probability);
        }

        const auto rolling_scores = make_oos_scores(predictions, samples, true);
        const auto ewma_scores = make_oos_scores(predictions, samples, false);
        const auto previous_abs_scores = make_previous_abs_scores(predictions, samples);
        const auto rolling_probabilities = make_fold_local_probabilities(predictions, samples, rolling_scores);
        const auto ewma_probabilities = make_fold_local_probabilities(predictions, samples, ewma_scores);
        const auto previous_abs_probabilities = make_fold_local_probabilities(
            predictions, samples, previous_abs_scores
        );
        const auto raw_metrics = calculate_metrics(labels, raw_probabilities);
        const auto calibrated_metrics = calculate_metrics(labels, calibrated_probabilities);
        const auto rolling_metrics = calculate_metrics(labels, rolling_probabilities);
        const auto ewma_metrics = calculate_metrics(labels, ewma_probabilities);
        const auto previous_abs_metrics = calculate_metrics(labels, previous_abs_probabilities);

        const auto model_auc_lower = bootstrap_auc_lower_bound(
            labels, raw_probabilities, options.block_length, options.bootstrap_samples, options.seed
        );
        const auto calibrated_auc_lower = bootstrap_auc_lower_bound(
            labels, calibrated_probabilities, options.block_length, options.bootstrap_samples, options.seed + 1U
        );
        const auto rolling_auc_lower = bootstrap_auc_lower_bound(
            labels, rolling_scores, options.block_length, options.bootstrap_samples, options.seed + 2U
        );
        const auto ewma_auc_lower = bootstrap_auc_lower_bound(
            labels, ewma_scores, options.block_length, options.bootstrap_samples, options.seed + 3U
        );
        const auto previous_abs_auc_lower = bootstrap_auc_lower_bound(
            labels,
            previous_abs_scores,
            options.block_length,
            options.bootstrap_samples,
            options.seed + 4U
        );

        const auto next_abs_returns = make_next_abs_returns(predictions, samples, market);
        std::array<double, 5> quintile_mean_abs_returns{};
        std::array<double, 5> quintile_high_rate{};
        for (std::size_t quintile = 0; quintile < 5; ++quintile) {
            const auto begin = quintile * next_abs_returns.size() / 5;
            const auto end = (quintile + 1) * next_abs_returns.size() / 5;
            std::vector<std::pair<double, double>> ranked_returns;
            ranked_returns.reserve(next_abs_returns.size());
            for (std::size_t index = 0; index < raw_probabilities.size(); ++index) {
                ranked_returns.emplace_back(raw_probabilities[index], next_abs_returns[index]);
            }
            std::ranges::sort(ranked_returns, {}, &std::pair<double, double>::first);
            double return_sum = 0.0;
            std::size_t high_count = 0;
            for (std::size_t index = begin; index < end; ++index) {
                return_sum += ranked_returns[index].second;
                if (ranked_returns[index].second > 0.02) ++high_count;
            }
            const auto count = static_cast<double>(end - begin);
            quintile_mean_abs_returns[quintile] = return_sum / count;
            quintile_high_rate[quintile] = static_cast<double>(high_count) / count;
        }

        double top_quartile_threshold = 0.0;
        std::string threshold_source = "current OOS diagnostic (not prospective)";
        if (!options.threshold_predictions.empty()) {
            const auto threshold_predictions = load_predictions(options.threshold_predictions);
            std::vector<double> threshold_probabilities;
            threshold_probabilities.reserve(threshold_predictions.size());
            for (const auto& prediction : threshold_predictions) {
                threshold_probabilities.push_back(prediction.raw_probability);
            }
            top_quartile_threshold = empirical_quantile(threshold_probabilities, 0.75);
            threshold_source = options.threshold_predictions.string();
        } else {
            top_quartile_threshold = empirical_quantile(raw_probabilities, 0.75);
        }

        double model_mean_exposure = 0.0;
        for (const auto probability : calibrated_probabilities) model_mean_exposure += clamp_exposure(1.0 - probability);
        model_mean_exposure /= static_cast<double>(calibrated_probabilities.size());
        std::vector<double> model_exposures;
        std::vector<double> matched_exposures;
        std::vector<double> ewma_exposures;
        std::vector<double> top_quartile_exposures;
        model_exposures.reserve(predictions.size());
        matched_exposures.reserve(predictions.size());
        ewma_exposures.reserve(predictions.size());
        top_quartile_exposures.reserve(predictions.size());
        for (std::size_t index = 0; index < calibrated_probabilities.size(); ++index) {
            const auto calibrated_probability = calibrated_probabilities[index];
            model_exposures.push_back(clamp_exposure(1.0 - calibrated_probability));
            matched_exposures.push_back(model_mean_exposure);
            top_quartile_exposures.push_back(
                raw_probabilities[index] >= top_quartile_threshold ? 0.5 : 1.0
            );
        }
        constexpr double annual_target_volatility = 0.15;
        for (const auto score : ewma_scores) {
            ewma_exposures.push_back(
                clamp_exposure(annual_target_volatility / (std::max(score, 1.0e-6) * std::sqrt(252.0)))
            );
        }

        StrategySet strategies;
        for (const auto cost : std::array<int, 4>{0, 5, 10, 20}) {
            strategies.model.emplace(
                cost, calculate_strategy(predictions, samples, market, model_exposures, cost)
            );
            strategies.matched_buy_and_hold.emplace(
                cost, calculate_strategy(predictions, samples, market, matched_exposures, cost)
            );
            strategies.ewma.emplace(
                cost, calculate_strategy(predictions, samples, market, ewma_exposures, cost)
            );
            strategies.top_quartile_overlay.emplace(
                cost,
                calculate_strategy(predictions, samples, market, top_quartile_exposures, cost)
            );
        }

        if (options.output.has_parent_path()) std::filesystem::create_directories(options.output.parent_path());
        std::ofstream output{options.output};
        if (!output) throw std::runtime_error{"Could not write evaluation output"};
        output << std::fixed << std::setprecision(8)
               << "{\n  \"protocol\": {\n"
               << "    \"predictions\": \"" << options.predictions.string() << "\",\n"
               << "    \"dataset\": \"" << options.dataset.string() << "\",\n"
               << "    \"market_data\": \"" << options.market_data.string() << "\",\n"
               << "    \"rolling_baseline\": \"20-session realized close-return volatility\",\n"
               << "    \"ewma_baseline\": \"open-to-close EWMA volatility, half-life 20 sessions\",\n"
               << "    \"previous_day_baseline\": \"previous-session absolute open-to-close return\",\n"
               << "    \"baseline_probability_fit\": \"fold-local logistic map fit through the end of each training window\",\n"
               << "    \"risk_overlay\": \"exposure=clamp(1-calibrated_high_vol_probability,0.25,1.0), next-session open-to-close return\",\n"
               << "    \"top_quartile_overlay\": \"100% exposure normally, 50% when raw model probability is above the development 75th percentile\",\n"
               << "    \"top_quartile_threshold_source\": \"" << threshold_source << "\",\n"
               << "    \"top_quartile_threshold\": " << top_quartile_threshold << ",\n"
               << "    \"costs_bps\": [0,5,10,20],\n"
               << "    \"bootstrap\": {\"type\": \"circular moving block\", \"block_length\": "
               << options.block_length << ", \"samples\": " << options.bootstrap_samples << "}\n"
               << "  },\n  \"rows\": " << predictions.size() << ",\n"
               << "  \"classification\": {\n    \"raw_model\": ";
        write_metrics(output, raw_metrics);
        output << ",\n    \"calibrated_model\": ";
        write_metrics(output, calibrated_metrics);
        output << ",\n    \"rolling_volatility\": ";
        write_metrics(output, rolling_metrics);
        output << ",\n    \"ewma_volatility\": ";
        write_metrics(output, ewma_metrics);
        output << ",\n    \"previous_day_abs_return\": ";
        write_metrics(output, previous_abs_metrics);
        output << "\n  },\n  \"bootstrap_auc_lower_2_5_percentile\": {\n"
               << "    \"raw_model\": " << model_auc_lower << ",\n"
               << "    \"calibrated_model\": " << calibrated_auc_lower << ",\n"
               << "    \"rolling_volatility\": " << rolling_auc_lower << ",\n"
               << "    \"ewma_volatility\": " << ewma_auc_lower << ",\n"
               << "    \"previous_day_abs_return\": " << previous_abs_auc_lower << "\n  },\n"
               << "  \"risk_quintiles\": {\n"
               << "    \"ordering\": \"ascending raw model risk score\",\n    \"mean_next_abs_open_to_close_return\": [";
        for (std::size_t quintile = 0; quintile < quintile_mean_abs_returns.size(); ++quintile) {
            if (quintile > 0) output << ", ";
            output << quintile_mean_abs_returns[quintile];
        }
        output << "],\n    \"fraction_next_abs_return_above_2_percent\": [";
        for (std::size_t quintile = 0; quintile < quintile_high_rate.size(); ++quintile) {
            if (quintile > 0) output << ", ";
            output << quintile_high_rate[quintile];
        }
        output << "]\n  },\n"
               << "  \"strategy\": {\n    \"model\": ";
        write_strategy(output, strategies.model);
        output << ",\n    \"exposure_matched_buy_and_hold\": ";
        write_strategy(output, strategies.matched_buy_and_hold);
        output << ",\n    \"ewma_risk_target\": ";
        write_strategy(output, strategies.ewma);
        output << ",\n    \"top_quartile_50_percent_overlay\": ";
        write_strategy(output, strategies.top_quartile_overlay);
        output << "\n  }\n}\n";

        std::cout << "Evaluated " << predictions.size() << " untouched rows; raw AUC="
                  << raw_metrics.auc << ", calibrated AUC=" << calibrated_metrics.auc
                  << ", EWMA AUC=" << ewma_metrics.auc << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-evaluate-volatility: " << error.what() << '\n';
        return 1;
    }
}
