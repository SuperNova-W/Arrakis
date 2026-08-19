// Backfills true end-of-day OHLCV bars from backend/data/history/<SYMBOL>.csv
// into etf_bars_daily.
//
// The live inference path used to rebuild "daily" closes and volumes by
// aggregating etf_bars_5m rows produced by the Finnhub stream.  That needed
// ~30 sessions of warm-up after a deploy and put volume on a completely
// different scale than the offline training dataset.  This loader seeds the
// same daily series the training dataset was built from, using the identical
// trading-date derivation (see include/arrakis/database/daily_bars.hpp).
//
// Usage:
//   daily-bar-loader [<history-dir>] [<symbol> ...]
// Defaults: history-dir = data/history (override with ARRAKIS_HISTORY_DIR),
//           symbols     = XLK SPY (override with ARRAKIS_DAILY_BAR_SYMBOLS,
//                                  a comma or space separated list).
// Re-running is safe: every row is an ON CONFLICT DO UPDATE upsert.

#include "arrakis/database/daily_bars.hpp"
#include "arrakis/database/postgres.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

[[nodiscard]] std::string env_value(const char* name, std::string fallback) {
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? fallback : std::string{value};
}

[[nodiscard]] std::vector<std::string> split_symbols(const std::string& value) {
    std::vector<std::string> symbols;
    std::string current;
    for (const char character : value) {
        if (character == ',' || character == ' ' || character == '\t') {
            if (!current.empty()) symbols.push_back(current);
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) symbols.push_back(current);
    return symbols;
}

[[nodiscard]] std::vector<arrakis::database::DailyBarRecord> read_history(
    const std::filesystem::path& path, const std::string& symbol, std::size_t& skipped) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open daily history file: " + path.string()};
    std::vector<arrakis::database::DailyBarRecord> records;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        auto record = arrakis::database::parse_daily_bar_csv_line(line);
        if (!record) {
            if (line_number > 1 && !line.empty()) ++skipped;
            continue;
        }
        // The file name is authoritative; the CSV symbol column is only a check.
        if (record->symbol != symbol) {
            ++skipped;
            continue;
        }
        records.push_back(std::move(*record));
    }
    return records;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::vector<std::string> arguments(argv + 1, argv + argc);
        std::filesystem::path history_dir = env_value("ARRAKIS_HISTORY_DIR", "data/history");
        if (!arguments.empty() && arguments.front().find('/') != std::string::npos) {
            history_dir = arguments.front();
            arguments.erase(arguments.begin());
        }
        auto symbols = arguments;
        if (symbols.empty()) symbols = split_symbols(env_value("ARRAKIS_DAILY_BAR_SYMBOLS", "XLK SPY"));
        if (symbols.empty()) throw std::invalid_argument{"No symbols to load"};

        arrakis::database::PostgresPool database{arrakis::database::database_config_from_environment()};
        if (!database.healthy()) throw std::runtime_error{"PostgreSQL is not reachable"};

        std::size_t total = 0;
        for (const auto& symbol : symbols) {
            const auto path = history_dir / (symbol + ".csv");
            std::size_t skipped = 0;
            const auto records = read_history(path, symbol, skipped);
            const auto written = database.upsert_daily_bars(records, "history-csv");
            total += written;
            std::cout << "{\"service\":\"daily-bar-loader\",\"symbol\":\"" << symbol << "\",\"file\":\""
                      << path.string() << "\",\"rows\":" << written << ",\"skipped\":" << skipped;
            if (!records.empty()) {
                std::cout << ",\"first_trading_date\":\"" << records.front().trading_date
                          << "\",\"last_trading_date\":\"" << records.back().trading_date << "\"";
            }
            std::cout << "}\n";
        }
        std::cout << "{\"service\":\"daily-bar-loader\",\"total_rows\":" << total << "}\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "{\"service\":\"daily-bar-loader\",\"fatal\":\"" << error.what() << "\"}\n";
        return EXIT_FAILURE;
    }
}
