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
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::array<std::string_view, 11> kSectorSymbols{
    "XLB", "XLC", "XLE", "XLF", "XLI", "XLK", "XLP", "XLRE", "XLU", "XLV", "XLY"};
constexpr std::array<std::string_view, 7> kContextSymbols{
    "SPY", "QQQ", "IWM", "TLT", "HYG", "GLD", "USO"};

struct History final {
    std::vector<std::string> dates;
    std::vector<double> opens;
    std::vector<double> highs;
    std::vector<double> lows;
    std::vector<double> closes;
    std::vector<double> volumes;
    std::unordered_map<std::string, std::size_t> index_by_date;
};

struct Universe final {
    std::vector<std::string> dates;
    std::unordered_map<std::string, History> histories;
};

struct VixHistory final {
    std::vector<std::string> dates;
    std::vector<double> closes;
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
    if (quoted) throw std::invalid_argument{"Unterminated CSV quote"};
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

[[nodiscard]] History load_history(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open history: " + path.string()};
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error{"History is empty: " + path.string()};
    History history;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split_csv_line(line);
        if (fields.size() < 7) throw std::runtime_error{"History row has too few fields"};
        const auto date = date_from_epoch(std::stoll(fields[1]));
        if (!history.dates.empty() && date <= history.dates.back()) {
            throw std::runtime_error{"History dates are not strictly increasing: " + path.string()};
        }
        history.index_by_date.emplace(date, history.dates.size());
        history.dates.push_back(date);
        history.opens.push_back(std::stod(fields[2]));
        history.highs.push_back(std::stod(fields[3]));
        history.lows.push_back(std::stod(fields[4]));
        history.closes.push_back(std::stod(fields[5]));
        history.volumes.push_back(std::stod(fields[6]));
    }
    if (history.dates.empty()) throw std::runtime_error{"History has no usable rows"};
    return history;
}

[[nodiscard]] Universe load_universe(const std::filesystem::path& history_dir) {
    Universe universe;
    for (const auto symbol : kSectorSymbols) {
        universe.histories.emplace(
            std::string{symbol}, load_history(history_dir / (std::string{symbol} + ".csv"))
        );
    }
    for (const auto symbol : kContextSymbols) {
        universe.histories.emplace(
            std::string{symbol}, load_history(history_dir / (std::string{symbol} + ".csv"))
        );
    }

    const auto& anchor = universe.histories.at("XLK");
    for (const auto& date : anchor.dates) {
        bool present = true;
        for (const auto& [symbol, history] : universe.histories) {
            if (!history.index_by_date.contains(date)) {
                present = false;
                break;
            }
            (void)symbol;
        }
        if (present) universe.dates.push_back(date);
    }
    if (universe.dates.size() < 500) throw std::runtime_error{"Too few common universe dates"};
    return universe;
}

[[nodiscard]] VixHistory load_vix_history(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open VIX history: " + path.string()};
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error{"VIX history is empty"};
    VixHistory result;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split_csv_line(line);
        if (fields.size() < 2 || fields[1].empty()) continue;
        const auto value = std::stod(fields[1]);
        if (!(value > 0.0) || !std::isfinite(value)) continue;
        if (!result.dates.empty() && fields[0] <= result.dates.back()) {
            throw std::runtime_error{"VIX dates are not strictly increasing"};
        }
        result.dates.push_back(fields[0]);
        result.closes.push_back(value);
    }
    if (result.dates.size() < 60) throw std::runtime_error{"VIX history has too few observations"};
    return result;
}

[[nodiscard]] std::size_t vix_index_on_or_before(
    const VixHistory& history,
    const std::string_view date
) {
    const auto found = std::ranges::upper_bound(history.dates, date);
    if (found == history.dates.begin()) {
        throw std::invalid_argument{"VIX history starts after feature date"};
    }
    return static_cast<std::size_t>(std::distance(history.dates.begin(), found - 1));
}

[[nodiscard]] double vix_value(
    const VixHistory& history,
    const std::string_view date,
    const std::size_t offset = 0
) {
    const auto current = vix_index_on_or_before(history, date);
    if (current < offset) throw std::invalid_argument{"Insufficient VIX history"};
    return history.closes[current - offset];
}

[[nodiscard]] double vix_log_return(
    const VixHistory& history,
    const std::string_view date,
    const std::size_t window
) {
    const auto current = vix_index_on_or_before(history, date);
    if (current < window) throw std::invalid_argument{"Insufficient VIX return history"};
    return std::log(history.closes[current] / history.closes[current - window]);
}

[[nodiscard]] const History& history(const Universe& universe, const std::string_view symbol) {
    return universe.histories.at(std::string{symbol});
}

[[nodiscard]] double close_at(
    const Universe& universe,
    const std::string_view symbol,
    const std::string& date
) {
    const auto& item = history(universe, symbol);
    return item.closes.at(item.index_by_date.at(date));
}

[[nodiscard]] double open_at(
    const Universe& universe,
    const std::string_view symbol,
    const std::string& date
) {
    const auto& item = history(universe, symbol);
    return item.opens.at(item.index_by_date.at(date));
}

[[nodiscard]] double high_at(
    const Universe& universe,
    const std::string_view symbol,
    const std::string& date
) {
    const auto& item = history(universe, symbol);
    return item.highs.at(item.index_by_date.at(date));
}

[[nodiscard]] double low_at(
    const Universe& universe,
    const std::string_view symbol,
    const std::string& date
) {
    const auto& item = history(universe, symbol);
    const auto index = item.index_by_date.at(date);
    return item.lows.at(index);
}

[[nodiscard]] double volume_at(
    const Universe& universe,
    const std::string_view symbol,
    const std::string& date
) {
    const auto& item = history(universe, symbol);
    return item.volumes.at(item.index_by_date.at(date));
}

[[nodiscard]] double log_return(
    const Universe& universe,
    const std::string_view symbol,
    const std::vector<std::string>& dates,
    const std::size_t position,
    const std::size_t window
) {
    if (position < window) throw std::invalid_argument{"Insufficient return history"};
    const auto current = close_at(universe, symbol, dates[position]);
    const auto previous = close_at(universe, symbol, dates[position - window]);
    return std::log(current / previous);
}

[[nodiscard]] double rolling_mean_return(
    const Universe& universe,
    const std::string_view symbol,
    const std::vector<std::string>& dates,
    const std::size_t position,
    const std::size_t window
) {
    double total = 0.0;
    for (std::size_t offset = 0; offset < window; ++offset) {
        total += log_return(universe, symbol, dates, position - offset, 1);
    }
    return total / static_cast<double>(window);
}

[[nodiscard]] double rolling_volatility(
    const Universe& universe,
    const std::string_view symbol,
    const std::vector<std::string>& dates,
    const std::size_t position,
    const std::size_t window
) {
    std::vector<double> returns;
    returns.reserve(window);
    for (std::size_t offset = 0; offset < window; ++offset) {
        returns.push_back(log_return(universe, symbol, dates, position - offset, 1));
    }
    const auto mean = std::accumulate(returns.begin(), returns.end(), 0.0) /
                      static_cast<double>(window);
    double squared = 0.0;
    for (const auto value : returns) squared += (value - mean) * (value - mean);
    return std::sqrt(squared / static_cast<double>(window - 1));
}

[[nodiscard]] double relative_volume(
    const Universe& universe,
    const std::string_view symbol,
    const std::vector<std::string>& dates,
    const std::size_t position,
    const std::size_t window
) {
    double mean = 0.0;
    for (std::size_t offset = 1; offset <= window; ++offset) {
        mean += volume_at(universe, symbol, dates[position - offset]);
    }
    return volume_at(universe, symbol, dates[position]) / std::max(mean / static_cast<double>(window), 1.0e-8);
}

[[nodiscard]] double sma_distance(
    const Universe& universe,
    const std::string_view symbol,
    const std::vector<std::string>& dates,
    const std::size_t position,
    const std::size_t window
) {
    double mean = 0.0;
    for (std::size_t offset = 0; offset < window; ++offset) {
        mean += close_at(universe, symbol, dates[position - offset]);
    }
    return close_at(universe, symbol, dates[position]) /
               (mean / static_cast<double>(window)) -
           1.0;
}

[[nodiscard]] double rolling_range(
    const Universe& universe,
    const std::string_view symbol,
    const std::vector<std::string>& dates,
    const std::size_t position,
    const std::size_t window
) {
    double total = 0.0;
    for (std::size_t offset = 0; offset < window; ++offset) {
        const auto& date = dates[position - offset];
        total += (high_at(universe, symbol, date) - low_at(universe, symbol, date)) /
                 close_at(universe, symbol, date);
    }
    return total / static_cast<double>(window);
}

[[nodiscard]] double sample_stddev(const std::vector<double>& values) {
    if (values.size() < 2) throw std::invalid_argument{"At least two values are required for dispersion"};
    const auto mean = std::accumulate(values.begin(), values.end(), 0.0) /
                      static_cast<double>(values.size());
    double squared = 0.0;
    for (const auto value : values) squared += (value - mean) * (value - mean);
    return std::sqrt(squared / static_cast<double>(values.size() - 1));
}

[[nodiscard]] double correlation(
    const Universe& universe,
    const std::string_view left,
    const std::string_view right,
    const std::vector<std::string>& dates,
    const std::size_t position,
    const std::size_t window
) {
    std::vector<double> x;
    std::vector<double> y;
    x.reserve(window);
    y.reserve(window);
    for (std::size_t offset = 0; offset < window; ++offset) {
        x.push_back(log_return(universe, left, dates, position - offset, 1));
        y.push_back(log_return(universe, right, dates, position - offset, 1));
    }
    const auto x_mean = std::accumulate(x.begin(), x.end(), 0.0) / static_cast<double>(window);
    const auto y_mean = std::accumulate(y.begin(), y.end(), 0.0) / static_cast<double>(window);
    double covariance = 0.0;
    double x_squared = 0.0;
    double y_squared = 0.0;
    for (std::size_t index = 0; index < window; ++index) {
        const auto dx = x[index] - x_mean;
        const auto dy = y[index] - y_mean;
        covariance += dx * dy;
        x_squared += dx * dx;
        y_squared += dy * dy;
    }
    return covariance / std::sqrt(std::max(x_squared * y_squared, 1.0e-20));
}

[[nodiscard]] double beta(
    const Universe& universe,
    const std::string_view left,
    const std::string_view right,
    const std::vector<std::string>& dates,
    const std::size_t position,
    const std::size_t window
) {
    std::vector<double> x;
    std::vector<double> y;
    x.reserve(window);
    y.reserve(window);
    for (std::size_t offset = 0; offset < window; ++offset) {
        x.push_back(log_return(universe, left, dates, position - offset, 1));
        y.push_back(log_return(universe, right, dates, position - offset, 1));
    }
    const auto x_mean = std::accumulate(x.begin(), x.end(), 0.0) / static_cast<double>(window);
    const auto y_mean = std::accumulate(y.begin(), y.end(), 0.0) / static_cast<double>(window);
    double covariance = 0.0;
    double variance = 0.0;
    for (std::size_t index = 0; index < window; ++index) {
        covariance += (x[index] - x_mean) * (y[index] - y_mean);
        variance += (y[index] - y_mean) * (y[index] - y_mean);
    }
    return covariance / std::max(variance, 1.0e-20);
}

[[nodiscard]] double cross_sectional_rank(const std::vector<double>& values, const std::size_t index) {
    std::vector<double> sorted = values;
    std::ranges::sort(sorted);
    const auto value = values[index];
    const auto lower = std::ranges::lower_bound(sorted, value);
    const auto upper = std::ranges::upper_bound(sorted, value);
    const auto average_rank =
        (static_cast<double>(std::distance(sorted.begin(), lower)) +
         static_cast<double>(std::distance(sorted.begin(), upper) - 1)) /
        2.0;
    return average_rank / static_cast<double>(sorted.size() - 1);
}

[[nodiscard]] std::vector<std::string> feature_names(
    const bool rich_market,
    const bool opening_gap_features,
    const bool long_market,
    const bool context_market
) {
    std::vector<std::string> names{
        "return_1", "return_3", "return_6", "return_12", "rolling_mean_return_6",
        "rolling_mean_return_12", "rolling_volatility_6", "rolling_volatility_12",
        "rolling_volatility_24", "relative_volume_20", "close_to_sma_12",
        "close_to_sma_24", "high_low_range_1", "rolling_high_low_range_12", "SPY_return_1",
        "SPY_return_3", "SPY_return_6", "QQQ_return_1", "QQQ_return_3", "QQQ_return_6",
        "IWM_return_1", "IWM_return_3", "IWM_return_6", "TLT_return_1", "TLT_return_6",
        "HYG_return_1", "HYG_return_6", "GLD_return_1", "GLD_return_6", "USO_return_1",
        "USO_return_6", "sector_minus_SPY_return_1", "sector_minus_SPY_return_3",
        "sector_minus_SPY_return_6", "sector_minus_QQQ_return_1", "sector_minus_QQQ_return_6",
        "sector_return_rank_1", "sector_return_rank_3", "sector_return_rank_6",
        "sector_volatility_rank_12", "rolling_correlation_SPY_24", "rolling_beta_SPY_24",
        "rolling_correlation_QQQ_24", "sector_id"};
    if (rich_market) {
        names.insert(names.end(), {
            "sector_intraday_return_1", "sector_overnight_gap_1",
            "cross_sectional_mean_return_1", "cross_sectional_dispersion_return_1",
            "cross_sectional_breadth_up_1", "cross_sectional_mean_return_3",
            "cross_sectional_dispersion_return_3", "cross_sectional_breadth_up_3",
            "cross_sectional_mean_return_6", "cross_sectional_dispersion_return_6",
            "cross_sectional_breadth_up_6", "SPY_intraday_return_1", "SPY_overnight_gap_1",
            "QQQ_intraday_return_1", "QQQ_overnight_gap_1"
        });
    }
    if (opening_gap_features) {
        names.insert(names.end(), {
            "sector_overnight_gap_1", "sector_minus_SPY_overnight_gap_1",
            "sector_minus_beta_SPY_overnight_gap_1", "SPY_overnight_gap_1",
            "QQQ_overnight_gap_1"
        });
    }
    if (long_market) {
        names.insert(names.end(), {
            "return_24", "return_60", "return_120", "return_252",
            "rolling_mean_return_24", "rolling_volatility_60",
            "close_to_sma_60", "close_to_sma_120", "close_to_sma_252",
            "SPY_return_24", "SPY_return_60", "QQQ_return_24", "QQQ_return_60",
            "sector_minus_SPY_return_24", "sector_minus_SPY_return_60"
        });
    }
    if (context_market) {
        names.insert(names.end(), {
            "vix_level", "vix_change_1", "vix_return_5", "vix_return_20"
        });
    }
    return names;
}

void write_feature_row(
    std::ostream& output,
    const std::string& row_key,
    const std::vector<double>& values,
    const bool label
) {
    output << row_key;
    for (const auto value : values) output << ',' << std::setprecision(12) << value;
    output << ',' << (label ? 1 : 0) << '\n';
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc < 4 || argc > 6) {
            std::cout << "Usage: arrakis-build-pooled-sector-dataset <history-dir> <output.csv> <from-year> [horizon-sessions] [rank-label]\n"
                         "  rank-label: top or bottom; default is positive excess return\n"
                         "  To build the final direction target, append close-direction; rich-direction adds\n"
                         "  leakage-safe gap, intraday, breadth, and dispersion features;\n"
                         "  open-gap-direction builds a 09:30 ET current-close-vs-prior-close target using only\n"
                         "  prior-close features and current-session opening gaps; intraday-direction uses\n"
                         "  the same 09:30 ET features with a current-close-vs-current-open target;\n"
                         "  long-direction adds leakage-safe 24/60/120/252-session trend features;\n"
                         "  context-direction adds VIX level and trailing changes;\n"
                         "  open-next-close-direction predicts the next close relative to the current open.\n";
            return 0;
        }
        const auto universe = load_universe(argv[1]);
        const auto output_path = std::filesystem::path{argv[2]};
        const auto from_year = std::stoi(argv[3]);
        const auto horizon = argc >= 5 ? static_cast<std::size_t>(std::stoull(argv[4])) : 1U;
        const std::string rank_label = argc == 6 ? argv[5] : "";
        const bool rich_market = rank_label == "rich-direction";
        const bool open_gap_direction = rank_label == "open-gap-direction";
        const bool intraday_direction = rank_label == "intraday-direction";
        const bool long_market = rank_label == "long-direction";
        const bool context_market = rank_label == "context-direction";
        const bool open_next_close_direction = rank_label == "open-next-close-direction";
        const bool opening_gap_features =
            open_gap_direction || intraday_direction || open_next_close_direction;
        const bool close_direction_target =
            rank_label == "close-direction" || rich_market || opening_gap_features || long_market ||
            context_market;
        if (horizon == 0) throw std::invalid_argument{"horizon-sessions must be positive"};
        if (!rank_label.empty() && rank_label != "top" && rank_label != "bottom" &&
            !close_direction_target) {
            throw std::invalid_argument{
                "rank-label must be top, bottom, close-direction, rich-direction, open-gap-direction, "
                "intraday-direction, long-direction, context-direction, or open-next-close-direction"
            };
        }
        if ((rich_market && (opening_gap_features || long_market || context_market)) ||
            (long_market && (opening_gap_features || context_market)) ||
            (context_market && opening_gap_features)) {
            throw std::invalid_argument{
                         "rich-direction, open-gap-direction, intraday-direction, long-direction, and context-direction "
                "are mutually exclusive with open-next-close-direction"
            };
        }
        const auto names = feature_names(
            rich_market, opening_gap_features, long_market, context_market
        );
        const auto vix = context_market
            ? load_vix_history(std::filesystem::path{argv[1]} / "VIX.csv")
            : VixHistory{};
        if (output_path.has_parent_path()) std::filesystem::create_directories(output_path.parent_path());
        std::ofstream output{output_path};
        if (!output) throw std::runtime_error{"Could not write pooled dataset"};
        output << "date";
        for (const auto& name : names) output << ',' << name;
        output << ",target_next_close_up\n";

        std::size_t rows = 0;
        const auto minimum_history = long_market ? 252U : 60U;
        for (std::size_t position = minimum_history; position + horizon < universe.dates.size(); ++position) {
            const auto& date = universe.dates[position];
            if (std::stoi(date.substr(0, 4)) < from_year) continue;
            const auto feature_position = opening_gap_features ? position - 1 : position;
            const auto& feature_date = universe.dates[feature_position];
            const auto& previous_date = universe.dates[position - 1];
            const auto& next_date = universe.dates[position + 1];
            const auto& future_date = universe.dates[position + horizon];
            std::vector<double> returns_1;
            std::vector<double> returns_3;
            std::vector<double> returns_6;
            std::vector<double> volatility_12;
            std::vector<double> intraday_returns;
            std::vector<double> overnight_gaps;
            returns_1.reserve(kSectorSymbols.size());
            returns_3.reserve(kSectorSymbols.size());
            returns_6.reserve(kSectorSymbols.size());
            volatility_12.reserve(kSectorSymbols.size());
            intraday_returns.reserve(kSectorSymbols.size());
            overnight_gaps.reserve(kSectorSymbols.size());
            for (const auto sector : kSectorSymbols) {
                returns_1.push_back(log_return(universe, sector, universe.dates, feature_position, 1));
                returns_3.push_back(log_return(universe, sector, universe.dates, feature_position, 3));
                returns_6.push_back(log_return(universe, sector, universe.dates, feature_position, 6));
                volatility_12.push_back(
                    rolling_volatility(universe, sector, universe.dates, feature_position, 12)
                );
                if (rich_market) {
                    const auto current_open = open_at(universe, sector, feature_date);
                    const auto previous_close = close_at(universe, sector, previous_date);
                    const auto current_close = close_at(universe, sector, feature_date);
                    if (!(current_open > 0.0) || !(previous_close > 0.0) || !(current_close > 0.0)) {
                        throw std::runtime_error{"Non-positive price in rich market feature row"};
                    }
                    intraday_returns.push_back(std::log(current_close / current_open));
                    overnight_gaps.push_back(std::log(current_open / previous_close));
                }
            }
            const auto spy_return_1 = log_return(universe, "SPY", universe.dates, feature_position, 1);
            const auto spy_return_3 = log_return(universe, "SPY", universe.dates, feature_position, 3);
            const auto spy_return_6 = log_return(universe, "SPY", universe.dates, feature_position, 6);
            const auto qqq_return_1 = log_return(universe, "QQQ", universe.dates, feature_position, 1);
            const auto qqq_return_3 = log_return(universe, "QQQ", universe.dates, feature_position, 3);
            const auto qqq_return_6 = log_return(universe, "QQQ", universe.dates, feature_position, 6);
            const auto context_return = [&](const std::string_view symbol, const std::size_t window) {
                return log_return(universe, symbol, universe.dates, feature_position, window);
            };
            const auto context_intraday = [&](const std::string_view symbol) {
                return std::log(close_at(universe, symbol, feature_date) / open_at(universe, symbol, feature_date));
            };
            const auto context_gap = [&](const std::string_view symbol) {
                return std::log(open_at(universe, symbol, feature_date) /
                                close_at(universe, symbol, previous_date));
            };
            const auto mean = [](const std::vector<double>& values) {
                return std::accumulate(values.begin(), values.end(), 0.0) /
                       static_cast<double>(values.size());
            };
            const auto breadth = [](const std::vector<double>& values) {
                return static_cast<double>(std::ranges::count_if(values, [](const double value) {
                    return value > 0.0;
                })) / static_cast<double>(values.size());
            };

            std::vector<double> future_excess_returns;
            if (!opening_gap_features) {
                future_excess_returns.reserve(kSectorSymbols.size());
                for (const auto sector : kSectorSymbols) {
                    const auto next_open = open_at(universe, sector, next_date);
                    const auto future_close = close_at(universe, sector, future_date);
                    future_excess_returns.push_back(
                        std::log(future_close / next_open) -
                        std::log(close_at(universe, "SPY", future_date) /
                                 open_at(universe, "SPY", next_date))
                    );
                }
            }

            for (std::size_t sector_index = 0; sector_index < kSectorSymbols.size(); ++sector_index) {
                const auto sector = kSectorSymbols[sector_index];
                double target = 0.0;
                double future_rank = 0.5;
                if (!opening_gap_features) {
                    const auto& next_open = open_at(universe, sector, next_date);
                    const auto& future_close = close_at(universe, sector, future_date);
                    const auto next_spy_open = open_at(universe, "SPY", next_date);
                    const auto future_spy_close = close_at(universe, "SPY", future_date);
                    target = std::log(future_close / next_open) -
                             std::log(future_spy_close / next_spy_open);
                    future_rank = cross_sectional_rank(future_excess_returns, sector_index);
                }
                const auto current_close = close_at(universe, sector, date);
                const auto label = open_gap_direction
                                   ? current_close > close_at(universe, sector, previous_date)
                                   : intraday_direction
                                   ? current_close > open_at(universe, sector, date)
                                   : open_next_close_direction
                                   ? close_at(universe, sector, future_date) >
                                         open_at(universe, sector, date)
                                   : close_direction_target
                                   ? close_at(universe, sector, future_date) > current_close
                                   : rank_label == "top" ? future_rank >= 0.7
                                   : rank_label == "bottom" ? future_rank <= 0.3
                                                             : target > 0.0;
                std::vector<double> values;
                values.reserve(names.size());
                const auto sector_return_1 = returns_1[sector_index];
                const auto sector_return_3 = returns_3[sector_index];
                const auto sector_return_6 = returns_6[sector_index];
                values.push_back(sector_return_1);
                values.push_back(sector_return_3);
                values.push_back(sector_return_6);
                values.push_back(log_return(universe, sector, universe.dates, feature_position, 12));
                values.push_back(rolling_mean_return(universe, sector, universe.dates, feature_position, 6));
                values.push_back(rolling_mean_return(universe, sector, universe.dates, feature_position, 12));
                values.push_back(rolling_volatility(universe, sector, universe.dates, feature_position, 6));
                values.push_back(rolling_volatility(universe, sector, universe.dates, feature_position, 12));
                values.push_back(rolling_volatility(universe, sector, universe.dates, feature_position, 24));
                values.push_back(relative_volume(universe, sector, universe.dates, feature_position, 20));
                values.push_back(sma_distance(universe, sector, universe.dates, feature_position, 12));
                values.push_back(sma_distance(universe, sector, universe.dates, feature_position, 24));
                values.push_back(
                    (high_at(universe, sector, feature_date) - low_at(universe, sector, feature_date)) /
                    close_at(universe, sector, feature_date)
                );
                values.push_back(rolling_range(universe, sector, universe.dates, feature_position, 12));
                values.push_back(spy_return_1);
                values.push_back(spy_return_3);
                values.push_back(spy_return_6);
                values.push_back(qqq_return_1);
                values.push_back(qqq_return_3);
                values.push_back(qqq_return_6);
                values.push_back(context_return("IWM", 1));
                values.push_back(context_return("IWM", 3));
                values.push_back(context_return("IWM", 6));
                values.push_back(context_return("TLT", 1));
                values.push_back(context_return("TLT", 6));
                values.push_back(context_return("HYG", 1));
                values.push_back(context_return("HYG", 6));
                values.push_back(context_return("GLD", 1));
                values.push_back(context_return("GLD", 6));
                values.push_back(context_return("USO", 1));
                values.push_back(context_return("USO", 6));
                values.push_back(sector_return_1 - spy_return_1);
                values.push_back(sector_return_3 - spy_return_3);
                values.push_back(sector_return_6 - spy_return_6);
                values.push_back(sector_return_1 - qqq_return_1);
                values.push_back(sector_return_6 - qqq_return_6);
                values.push_back(cross_sectional_rank(returns_1, sector_index));
                values.push_back(cross_sectional_rank(returns_3, sector_index));
                values.push_back(cross_sectional_rank(returns_6, sector_index));
                values.push_back(cross_sectional_rank(volatility_12, sector_index));
                values.push_back(correlation(universe, sector, "SPY", universe.dates, feature_position, 24));
                values.push_back(beta(universe, sector, "SPY", universe.dates, feature_position, 24));
                values.push_back(correlation(universe, sector, "QQQ", universe.dates, feature_position, 24));
                values.push_back(static_cast<double>(sector_index));
                if (rich_market) {
                    values.push_back(intraday_returns[sector_index]);
                    values.push_back(overnight_gaps[sector_index]);
                    values.push_back(mean(returns_1));
                    values.push_back(sample_stddev(returns_1));
                    values.push_back(breadth(returns_1));
                    values.push_back(mean(returns_3));
                    values.push_back(sample_stddev(returns_3));
                    values.push_back(breadth(returns_3));
                    values.push_back(mean(returns_6));
                    values.push_back(sample_stddev(returns_6));
                    values.push_back(breadth(returns_6));
                    values.push_back(context_intraday("SPY"));
                    values.push_back(context_gap("SPY"));
                    values.push_back(context_intraday("QQQ"));
                    values.push_back(context_gap("QQQ"));
                }
                if (opening_gap_features) {
                    const auto sector_gap = std::log(
                        open_at(universe, sector, date) / close_at(universe, sector, previous_date)
                    );
                    const auto spy_gap = std::log(
                        open_at(universe, "SPY", date) / close_at(universe, "SPY", previous_date)
                    );
                    const auto qqq_gap = std::log(
                        open_at(universe, "QQQ", date) / close_at(universe, "QQQ", previous_date)
                    );
                    const auto sector_beta = beta(
                        universe, sector, "SPY", universe.dates, feature_position, 24
                    );
                    values.push_back(sector_gap);
                    values.push_back(sector_gap - spy_gap);
                    values.push_back(sector_gap - sector_beta * spy_gap);
                    values.push_back(spy_gap);
                    values.push_back(qqq_gap);
                }
                if (long_market) {
                    const auto sector_return_24 =
                        log_return(universe, sector, universe.dates, feature_position, 24);
                    const auto sector_return_60 =
                        log_return(universe, sector, universe.dates, feature_position, 60);
                    const auto sector_return_120 =
                        log_return(universe, sector, universe.dates, feature_position, 120);
                    const auto sector_return_252 =
                        log_return(universe, sector, universe.dates, feature_position, 252);
                    const auto spy_return_24 =
                        log_return(universe, "SPY", universe.dates, feature_position, 24);
                    const auto spy_return_60 =
                        log_return(universe, "SPY", universe.dates, feature_position, 60);
                    const auto qqq_return_24 =
                        log_return(universe, "QQQ", universe.dates, feature_position, 24);
                    const auto qqq_return_60 =
                        log_return(universe, "QQQ", universe.dates, feature_position, 60);
                    values.push_back(sector_return_24);
                    values.push_back(sector_return_60);
                    values.push_back(sector_return_120);
                    values.push_back(sector_return_252);
                    values.push_back(
                        rolling_mean_return(universe, sector, universe.dates, feature_position, 24)
                    );
                    values.push_back(
                        rolling_volatility(universe, sector, universe.dates, feature_position, 60)
                    );
                    values.push_back(
                        sma_distance(universe, sector, universe.dates, feature_position, 60)
                    );
                    values.push_back(
                        sma_distance(universe, sector, universe.dates, feature_position, 120)
                    );
                    values.push_back(
                        sma_distance(universe, sector, universe.dates, feature_position, 252)
                    );
                    values.push_back(spy_return_24);
                    values.push_back(spy_return_60);
                    values.push_back(qqq_return_24);
                    values.push_back(qqq_return_60);
                    values.push_back(sector_return_24 - spy_return_24);
                    values.push_back(sector_return_60 - spy_return_60);
                }
                if (context_market) {
                    const auto current_vix = vix_value(vix, feature_date);
                    const auto previous_vix = vix_value(vix, feature_date, 1);
                    values.push_back(current_vix);
                    values.push_back(current_vix / previous_vix - 1.0);
                    values.push_back(vix_log_return(vix, feature_date, 5));
                    values.push_back(vix_log_return(vix, feature_date, 20));
                }
                if (values.size() != names.size() || !std::ranges::all_of(values, [](const double value) {
                        return std::isfinite(value);
                    })) {
                    throw std::runtime_error{"Pooled feature row is invalid"};
                }
                write_feature_row(output, date + "|" + std::string{sector}, values, label);
                ++rows;
            }
        }
        std::cout << "Wrote " << rows << " pooled sector rows to " << output_path
                  << " with horizon_sessions=" << horizon << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-build-pooled-sector-dataset: " << error.what() << '\n';
        return 1;
    }
}
