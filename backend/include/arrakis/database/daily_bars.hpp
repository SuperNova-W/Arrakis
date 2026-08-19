#pragma once

// Pure, database-free helpers shared by the daily-bar backfill loader
// (src/daily_bar_loader.cpp) and the point-in-time daily-bar reader
// (libs/database/src/postgres.cpp).
//
// Trading-date convention
// -----------------------
// The offline training dataset (services/news_nlp/src/build_xlk_combined_dataset.cpp,
// load_market) derives the trading date of a `backend/data/history/<SYMBOL>.csv`
// row by formatting the unix-second timestamp as a **UTC** calendar date
// (gmtime_r + "%Y-%m-%d").  Every row in those files carries a session-open
// timestamp of 13:30 UTC (US daylight time) or 14:30 UTC (US standard time),
// i.e. 09:30 America/New_York, so the UTC calendar date and the
// America/New_York trading date are always identical for this data.  We
// reproduce the training-time derivation exactly so the backfilled
// `etf_bars_daily.trading_date` keys match the dataset the model was fit on.
//
// Availability convention
// -----------------------
// A daily bar for trading date D only becomes known at D's regular session
// close, 16:00 America/New_York.  `session_close_unix_ms` reproduces the same
// DST rule used by `market_close_ms` in build_xlk_combined_dataset.cpp
// (20:00 UTC during US daylight time, 21:00 UTC otherwise).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace arrakis::database {

// A completed-or-partial daily observation as consumed by the news market
// feature vector.
struct DailyMarketBar final {
    std::string trading_date;
    double close{};
    double volume{};
};

// A full daily OHLCV row destined for etf_bars_daily.
struct DailyBarRecord final {
    std::string symbol;
    std::string trading_date;
    double open{};
    double high{};
    double low{};
    double close{};
    double volume{};
};

// UTC calendar date of a unix-second epoch, formatted YYYY-MM-DD.
// Matches build_xlk_combined_dataset.cpp::load_market.
[[nodiscard]] inline std::string trading_date_from_unix_seconds(std::int64_t seconds) {
    const auto value = static_cast<std::time_t>(seconds);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    char buffer[11]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &utc) == 0) {
        throw std::invalid_argument{"Could not format trading date for epoch " + std::to_string(seconds)};
    }
    return std::string{buffer};
}

// True when `date` falls inside the US daylight-saving window (second Sunday in
// March through the day before the first Sunday in November).
[[nodiscard]] inline bool is_us_daylight_date(int year, int month, int day) {
    std::tm march{};
    march.tm_year = year - 1900;
    march.tm_mon = 2;
    march.tm_mday = 1;
    const auto march_epoch = timegm(&march);
    std::tm march_utc{};
    gmtime_r(&march_epoch, &march_utc);
    const auto second_sunday = 1 + ((7 - march_utc.tm_wday) % 7) + 7;

    std::tm november{};
    november.tm_year = year - 1900;
    november.tm_mon = 10;
    november.tm_mday = 1;
    const auto november_epoch = timegm(&november);
    std::tm november_utc{};
    gmtime_r(&november_epoch, &november_utc);
    const auto first_sunday = 1 + ((7 - november_utc.tm_wday) % 7);

    return (month > 3 && month < 11) || (month == 3 && day >= second_sunday) ||
           (month == 11 && day < first_sunday);
}

// Unix milliseconds of the 16:00 America/New_York regular session close for a
// YYYY-MM-DD trading date.  Mirrors build_xlk_combined_dataset.cpp::market_close_ms.
[[nodiscard]] inline std::int64_t session_close_unix_ms(std::string_view date) {
    if (date.size() < 10) throw std::invalid_argument{"Invalid trading date: " + std::string{date}};
    const std::string value{date};
    const auto year = std::stoi(value.substr(0, 4));
    const auto month = std::stoi(value.substr(5, 2));
    const auto day = std::stoi(value.substr(8, 2));
    std::tm close{};
    close.tm_year = year - 1900;
    close.tm_mon = month - 1;
    close.tm_mday = day;
    close.tm_hour = is_us_daylight_date(year, month, day) ? 20 : 21;
    return static_cast<std::int64_t>(timegm(&close)) * 1000;
}

// Leakage boundary: an end-of-day bar for `date` may only be used by a
// consumer whose point-in-time cutoff is at or after that session's close.
[[nodiscard]] inline bool daily_bar_visible_at(std::string_view date, std::int64_t cutoff_unix_ms) {
    return session_close_unix_ms(date) <= cutoff_unix_ms;
}

// Splits one CSV record, honouring double quotes and "" escapes.
[[nodiscard]] inline std::vector<std::string> split_daily_bar_csv(std::string_view line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char character = line[i];
        if (character == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
                field.push_back('"');
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (character == ',' && !quoted) {
            fields.push_back(field);
            field.clear();
        } else if (character == '\r' && !quoted) {
            continue;
        } else {
            field.push_back(character);
        }
    }
    if (quoted) throw std::invalid_argument{"Unterminated CSV quote in daily bar history"};
    fields.push_back(field);
    return fields;
}

// Parses one `symbol,timestamp_utc,open,high,low,close,volume` history row.
// Returns nullopt for blank lines, header rows and short records.
[[nodiscard]] inline std::optional<DailyBarRecord> parse_daily_bar_csv_line(std::string_view line) {
    if (line.empty()) return std::nullopt;
    const auto fields = split_daily_bar_csv(line);
    if (fields.size() < 7) return std::nullopt;
    if (fields[0].empty() || fields[0] == "symbol") return std::nullopt;
    DailyBarRecord record;
    record.symbol = fields[0];
    std::ranges::transform(record.symbol, record.symbol.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    try {
        record.trading_date = trading_date_from_unix_seconds(std::stoll(fields[1]));
        record.open = std::stod(fields[2]);
        record.high = std::stod(fields[3]);
        record.low = std::stod(fields[4]);
        record.close = std::stod(fields[5]);
        record.volume = std::stod(fields[6]);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (record.open <= 0 || record.high <= 0 || record.low <= 0 || record.close <= 0 || record.volume < 0) {
        return std::nullopt;
    }
    if (record.low > record.open || record.open > record.high) return std::nullopt;
    if (record.low > record.close || record.close > record.high) return std::nullopt;
    return record;
}

// Keeps only the end-of-day bars whose session close is at or before the
// cutoff.  Input need not be sorted; output is ascending by trading date.
[[nodiscard]] inline std::vector<DailyMarketBar> filter_completed_sessions(
    std::vector<DailyMarketBar> bars, std::int64_t cutoff_unix_ms) {
    std::erase_if(bars, [&](const DailyMarketBar& bar) {
        return !daily_bar_visible_at(bar.trading_date, cutoff_unix_ms);
    });
    std::ranges::sort(bars, {}, &DailyMarketBar::trading_date);
    return bars;
}

// Appends the still-open session(s) reconstructed from the intraday stream,
// but never overrides a completed end-of-day bar for the same date.
[[nodiscard]] inline std::vector<DailyMarketBar> merge_daily_and_intraday(
    std::vector<DailyMarketBar> completed, const std::vector<DailyMarketBar>& intraday) {
    for (const auto& bar : intraday) {
        const bool present = std::ranges::any_of(completed, [&](const DailyMarketBar& existing) {
            return existing.trading_date == bar.trading_date;
        });
        if (!present) completed.push_back(bar);
    }
    std::ranges::sort(completed, {}, &DailyMarketBar::trading_date);
    return completed;
}

}  // namespace arrakis::database
