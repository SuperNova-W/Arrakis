#include "arrakis/news/market_features.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Bar final {
    double close{};
    double volume{};
};

std::vector<std::string> split_csv(const std::string& line) {
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
    if (quoted) throw std::runtime_error{"Unterminated CSV quote"};
    fields.push_back(field);
    return fields;
}

std::string market_date(const std::string& timestamp) {
    const auto seconds = static_cast<time_t>(std::stoll(timestamp));
    std::tm utc{};
    if (gmtime_r(&seconds, &utc) == nullptr) throw std::runtime_error{"Could not parse market timestamp"};
    char date[11]{};
    if (std::strftime(date, sizeof(date), "%Y-%m-%d", &utc) == 0) {
        throw std::runtime_error{"Could not format market timestamp"};
    }
    return date;
}

std::map<std::string, Bar> load_history(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open market history: " + path.string()};
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error{"Market history is empty: " + path.string()};
    std::map<std::string, Bar> result;
    while (std::getline(input, line)) {
        const auto fields = split_csv(line);
        if (fields.size() < 7) continue;
        result[market_date(fields[1])] = Bar{std::stod(fields[5]), std::stod(fields[6])};
    }
    return result;
}

std::vector<arrakis::news::MarketDay> market_days(const std::map<std::string, Bar>& history) {
    std::vector<arrakis::news::MarketDay> days;
    days.reserve(history.size());
    for (const auto& [date, bar] : history) days.push_back({date, bar.close, bar.volume});
    return days;
}

void write_row(std::ostream& output, const std::vector<std::string>& fields) {
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0) output << ',';
        output << fields[index];
    }
    output << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4) {
            throw std::invalid_argument{
                "Usage: arrakis-repair-xlk-market-features <combined.csv> <history-dir> <output.csv>"};
        }
        const auto input_path = std::filesystem::path{argv[1]};
        const auto history_dir = std::filesystem::path{argv[2]};
        const auto output_path = std::filesystem::path{argv[3]};
        const auto xlk_days = market_days(load_history(history_dir / "XLK.csv"));
        const auto spy_days = market_days(load_history(history_dir / "SPY.csv"));

        std::ifstream input{input_path};
        if (!input) throw std::runtime_error{"Could not open combined dataset: " + input_path.string()};
        std::string line;
        if (!std::getline(input, line)) throw std::runtime_error{"Combined dataset is empty"};
        const auto header = split_csv(line);
        if (header.size() < arrakis::news::kMarketFeatureCount + 2 || header.front() != "date") {
            throw std::runtime_error{"Combined dataset does not contain the expected market feature prefix"};
        }

        std::filesystem::create_directories(output_path.parent_path());
        std::ofstream output{output_path};
        if (!output) throw std::runtime_error{"Could not create repaired dataset: " + output_path.string()};
        write_row(output, header);
        std::size_t emitted = 0;
        std::size_t skipped = 0;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            auto fields = split_csv(line);
            if (fields.size() != header.size()) throw std::runtime_error{"Combined dataset row has an unexpected width"};
            const auto market_features = arrakis::news::market_feature_vector(xlk_days, spy_days, fields[0]);
            if (!market_features) {
                ++skipped;
                continue;
            }
            std::ostringstream value;
            value << std::setprecision(12);
            for (std::size_t index = 0; index < market_features->size(); ++index) {
                value.str("");
                value.clear();
                value << std::setprecision(12) << (*market_features)[index];
                fields[index + 1] = value.str();
            }
            write_row(output, fields);
            ++emitted;
        }
        std::cout << "Repaired XLK market features: emitted=" << emitted << " skipped=" << skipped << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "XLK market feature repair failed: " << error.what() << '\n';
        return 1;
    }
}
