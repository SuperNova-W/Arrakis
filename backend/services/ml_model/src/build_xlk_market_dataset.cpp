#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Bar final {
    double open{};
    double high{};
    double low{};
    double close{};
    double volume{};
};

using Series = std::map<std::string, Bar>;

[[nodiscard]] std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        if (line[index] == '"') {
            if (quoted && index + 1 < line.size() && line[index + 1] == '"') {
                field.push_back('"');
                ++index;
            } else {
                quoted = !quoted;
            }
        } else if (line[index] == ',' && !quoted) {
            fields.push_back(field);
            field.clear();
        } else {
            field.push_back(line[index]);
        }
    }
    if (quoted) throw std::runtime_error{"Market CSV ended inside a quoted field"};
    fields.push_back(field);
    return fields;
}

[[nodiscard]] Series load_series(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open market history: " + path.string()};
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error{"Market history is empty: " + path.string()};
    Series series;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split_csv(line);
        if (fields.size() < 7) throw std::runtime_error{"Market row has fewer than seven fields"};
        const auto timestamp = static_cast<std::time_t>(std::stoll(fields[1]));
        std::tm utc{};
        if (gmtime_r(&timestamp, &utc) == nullptr) throw std::runtime_error{"Could not convert market timestamp"};
        char date[11]{};
        if (std::strftime(date, sizeof(date), "%Y-%m-%d", &utc) == 0) {
            throw std::runtime_error{"Could not format market date"};
        }
        const auto [_, inserted] = series.emplace(
            date,
            Bar{
                .open = std::stod(fields[2]), .high = std::stod(fields[3]),
                .low = std::stod(fields[4]), .close = std::stod(fields[5]),
                .volume = std::stod(fields[6])
            }
        );
        if (!inserted) throw std::runtime_error{"Duplicate market session date: " + std::string{date}};
    }
    if (series.empty()) throw std::runtime_error{"Market history has no rows: " + path.string()};
    return series;
}

[[nodiscard]] double sample_stddev(const std::vector<double>& values) {
    if (values.size() < 2) throw std::invalid_argument{"At least two returns are required"};
    const auto mean = std::accumulate(values.begin(), values.end(), 0.0) /
        static_cast<double>(values.size());
    double squared = 0.0;
    for (const auto value : values) squared += (value - mean) * (value - mean);
    return std::sqrt(squared / static_cast<double>(values.size() - 1));
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 5) {
            std::cout << "Usage: arrakis-build-xlk-market-dataset <history_dir> <output.csv> "
                         "<from-date> <to-date>\n";
            return 0;
        }
        const auto history_dir = std::filesystem::path{argv[1]};
        const auto output_path = std::filesystem::path{argv[2]};
        const std::string from_date{argv[3]};
        const std::string to_date{argv[4]};
        if (from_date.empty() || to_date.empty() || from_date > to_date) {
            throw std::invalid_argument{"Invalid date range"};
        }
        const auto xlk = load_series(history_dir / "XLK.csv");
        const auto spy = load_series(history_dir / "SPY.csv");
        std::vector<std::pair<std::string, const Bar*>> dates;
        for (const auto& [date, bar] : xlk) {
            if (date >= from_date && date <= to_date) dates.emplace_back(date, &bar);
        }
        if (dates.size() < 20) throw std::runtime_error{"Not enough XLK sessions in requested range"};

        std::filesystem::create_directories(output_path.parent_path());
        std::ofstream output{output_path};
        if (!output) throw std::runtime_error{"Could not write market dataset"};
        output << std::setprecision(15)
               << "date,ret_1,ret_3,ret_6,volatility_6,volume_mean_6,rel_volume,rsi_14,"
                  "spy_ret_1,sector_spy_diff,target_next_close_up\n";

        std::size_t rows_written = 0;
        for (std::size_t index = 14; index + 1 < dates.size(); ++index) {
            const auto& date = dates[index].first;
            const auto& current = *dates[index].second;
            const auto& previous = *dates[index - 1].second;
            const auto& previous_three = *dates[index - 3].second;
            const auto& previous_six = *dates[index - 6].second;
            const auto spy_current = spy.find(date);
            const auto spy_previous = spy.find(dates[index - 1].first);
            if (spy_current == spy.end() || spy_previous == spy.end()) continue;
            if (!(current.close > 0.0) || !(previous.close > 0.0) ||
                !(previous_three.close > 0.0) || !(previous_six.close > 0.0) ||
                !(spy_current->second.close > 0.0) || !(spy_previous->second.close > 0.0)) {
                throw std::runtime_error{"Non-positive close in market history"};
            }

            std::vector<double> log_returns;
            log_returns.reserve(6);
            for (std::size_t trailing = index - 5; trailing <= index; ++trailing) {
                const auto& left = *dates[trailing - 1].second;
                const auto& right = *dates[trailing].second;
                log_returns.push_back(std::log(right.close / left.close));
            }
            double volume_sum = 0.0;
            for (std::size_t trailing = index - 5; trailing <= index; ++trailing) {
                volume_sum += dates[trailing].second->volume;
            }
            const auto volume_mean = volume_sum / 6.0;
            double average_gain = 0.0;
            double average_loss = 0.0;
            for (std::size_t trailing = index - 13; trailing <= index; ++trailing) {
                const auto change = dates[trailing].second->close - dates[trailing - 1].second->close;
                average_gain += std::max(0.0, change);
                average_loss += std::max(0.0, -change);
            }
            average_gain /= 14.0;
            average_loss /= 14.0;
            const auto rsi = average_loss == 0.0
                ? (average_gain == 0.0 ? 50.0 : 100.0)
                : 100.0 - (100.0 / (1.0 + average_gain / average_loss));
            const auto ret_1 = current.close / previous.close - 1.0;
            const auto ret_3 = current.close / previous_three.close - 1.0;
            const auto ret_6 = current.close / previous_six.close - 1.0;
            const auto spy_ret_1 = spy_current->second.close / spy_previous->second.close - 1.0;
            output << date << ',' << ret_1 << ',' << ret_3 << ',' << ret_6 << ','
                   << sample_stddev(log_returns) << ',' << volume_mean << ','
                   << current.volume / std::max(1.0, volume_mean) << ',' << rsi << ','
                   << spy_ret_1 << ',' << ret_1 - spy_ret_1 << ','
                   << (dates[index + 1].second->close > current.close ? 1 : 0) << '\n';
            ++rows_written;
        }

        std::ofstream manifest{std::string{output_path} + ".manifest.json"};
        if (!manifest) throw std::runtime_error{"Could not write market dataset manifest"};
        manifest << "{\n"
                 << "  \"source_history\": \"" << history_dir.string() << "\",\n"
                 << "  \"from_date\": \"" << from_date << "\",\n"
                 << "  \"to_date\": \"" << to_date << "\",\n"
                 << "  \"rows_written\": " << rows_written << ",\n"
                 << "  \"target\": \"target_next_close_up\",\n"
                 << "  \"target_policy\": \"close[t+1] > close[t]; features use data through close[t]\",\n"
                 << "  \"features\": [\"ret_1\",\"ret_3\",\"ret_6\",\"volatility_6\",\"volume_mean_6\",\"rel_volume\",\"rsi_14\",\"spy_ret_1\",\"sector_spy_diff\"]\n}\n";
        std::cout << "Wrote " << rows_written << " full-history XLK market rows\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-build-xlk-market-dataset: " << error.what() << '\n';
        return 1;
    }
}
