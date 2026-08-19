#include <openssl/sha.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

struct CsvReader final {
    std::ifstream input;

    explicit CsvReader(const std::filesystem::path& path) : input{path} {
        if (!input) throw std::runtime_error{"Could not open CSV: " + path.string()};
    }

    [[nodiscard]] std::vector<std::string> next() {
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
};

[[nodiscard]] std::size_t column_index(
    const std::vector<std::string>& header,
    const std::string_view name
) {
    const auto found = std::ranges::find(header, name);
    if (found == header.end()) throw std::runtime_error{"Missing CSV column: " + std::string{name}};
    return static_cast<std::size_t>(std::distance(header.begin(), found));
}

[[nodiscard]] std::string csv_escape(const std::string& value) {
    if (value.find_first_of(",\"\n\r") == std::string::npos) return value;
    std::string escaped{"\""};
    for (const auto character : value) {
        escaped.push_back(character);
        if (character == '"') escaped.push_back('"');
    }
    escaped.push_back('"');
    return escaped;
}

[[nodiscard]] std::string sha256(const std::string_view value) {
    unsigned char digest[SHA256_DIGEST_LENGTH]{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
}

[[nodiscard]] bool valid_date(const std::string_view value) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    for (std::size_t index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U}) {
        if (value[index] < '0' || value[index] > '9') return false;
    }
    return true;
}

[[nodiscard]] std::int64_t days_from_civil(
    const int year,
    const unsigned month,
    const unsigned day
) {
    const auto adjusted_year = static_cast<std::int64_t>(year) - (month <= 2U ? 1 : 0);
    const auto era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const auto year_of_era = adjusted_year - era * 400;
    const auto month_number = static_cast<std::int64_t>(month);
    const auto day_of_year = (153 * (month_number + (month_number > 2 ? -3 : 9)) + 2) / 5 +
        static_cast<std::int64_t>(day) - 1;
    const auto day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

[[nodiscard]] std::string date_string(const int year, const unsigned month, const unsigned day) {
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << year << '-' << std::setw(2) << month << '-'
           << std::setw(2) << day;
    return output.str();
}

[[nodiscard]] std::string date_from_epoch_seconds(const std::int64_t seconds) {
    const auto days = seconds / 86400;
    for (int year = 1970; year <= 2100; ++year) {
        for (unsigned month = 1; month <= 12; ++month) {
            const auto first = days_from_civil(year, month, 1);
            const auto next = month == 12
                ? days_from_civil(year + 1, 1, 1)
                : days_from_civil(year, month + 1, 1);
            if (days >= first && days < next) {
                return date_string(year, month, static_cast<unsigned>(days - first + 1));
            }
        }
    }
    throw std::runtime_error{"Market calendar timestamp is outside supported range"};
}

struct Calendar final {
    std::vector<std::string> dates;

    [[nodiscard]] std::string first_session_after(const std::string_view source_date) const {
        const auto found = std::ranges::upper_bound(dates, source_date);
        return found == dates.end() ? std::string{} : *found;
    }
};

[[nodiscard]] Calendar load_calendar(const std::filesystem::path& path) {
    CsvReader reader{path};
    const auto header = reader.next();
    const auto timestamp = column_index(header, "timestamp_utc");
    Calendar calendar;
    while (true) {
        const auto row = reader.next();
        if (row.empty()) break;
        if (row.size() <= timestamp) throw std::runtime_error{"Malformed market calendar row"};
        calendar.dates.push_back(date_from_epoch_seconds(std::stoll(row[timestamp])));
    }
    if (calendar.dates.empty()) throw std::runtime_error{"Market calendar is empty"};
    std::ranges::sort(calendar.dates);
    calendar.dates.erase(std::ranges::unique(calendar.dates).begin(), calendar.dates.end());
    return calendar;
}

struct Membership final {
    std::string sector;
    std::string effective_from;
    std::string available_from;
    std::string effective_to;
};

struct MembershipIndex final {
    std::unordered_map<std::string, std::vector<Membership>> by_symbol;

    [[nodiscard]] std::vector<std::string> sectors_for(
        const std::string_view symbol,
        const std::string_view date
    ) const {
        const auto found_symbol = by_symbol.find(std::string{symbol});
        if (found_symbol == by_symbol.end()) return {};
        std::map<std::string, const Membership*> best;
        for (const auto& row : found_symbol->second) {
            if (row.available_from >= date || row.effective_from > date || row.effective_to < date) continue;
            const auto found = best.find(row.sector);
            if (found == best.end() || row.available_from > found->second->available_from) {
                best[row.sector] = &row;
            }
        }
        std::vector<std::string> result;
        result.reserve(best.size());
        for (const auto& [sector, _] : best) result.push_back(sector);
        return result;
    }
};

[[nodiscard]] MembershipIndex load_membership(const std::filesystem::path& path) {
    CsvReader reader{path};
    const auto header = reader.next();
    const auto sector = column_index(header, "sector");
    const auto symbol = column_index(header, "symbol");
    const auto effective_from = column_index(header, "effective_from");
    const auto available_from = column_index(header, "available_from");
    const auto effective_to = column_index(header, "effective_to");
    MembershipIndex result;
    while (true) {
        const auto row = reader.next();
        if (row.empty()) break;
        if (row.size() != header.size()) throw std::runtime_error{"Malformed sector holdings row"};
        result.by_symbol[row[symbol]].push_back(Membership{
            .sector = row[sector],
            .effective_from = row[effective_from],
            .available_from = row[available_from],
            .effective_to = row[effective_to]
        });
    }
    if (result.by_symbol.empty()) throw std::runtime_error{"Sector holdings history is empty"};
    return result;
}

struct OutputRow final {
    std::string article_id;
    std::string published_at_utc;
    std::string trading_date;
    std::string sector;
    std::string symbol;
    std::string text;
    std::string url;
    std::string publisher;
    std::string content_hash;
};

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 7) {
            std::cout << "Usage: arrakis-build-yahoo-sector-articles <stock_news.csv> "
                         "<sector_holdings_history.csv> <market_calendar.csv> <from-date> "
                         "<to-date> <output.csv>\n";
            return 0;
        }
        const auto from_date = std::string{argv[4]};
        const auto to_date = std::string{argv[5]};
        if (!valid_date(from_date) || !valid_date(to_date) || from_date > to_date) {
            throw std::invalid_argument{"Invalid date range"};
        }
        const auto calendar = load_calendar(argv[3]);
        const auto membership = load_membership(argv[2]);
        CsvReader reader{argv[1]};
        const auto header = reader.next();
        const auto uuid_index = column_index(header, "uuid");
        const auto symbol_index = column_index(header, "symbol");
        const auto title_index = column_index(header, "title");
        const auto publisher_index = column_index(header, "publisher");
        const auto date_index = column_index(header, "report_date");
        const auto link_index = column_index(header, "link");
        const auto article_index = column_index(header, "article");

        std::size_t rows_read = 0;
        std::size_t invalid_dates = 0;
        std::size_t outside_range = 0;
        std::size_t no_future_session = 0;
        std::size_t membership_rejects = 0;
        std::size_t rows_written = 0;
        std::unordered_map<std::string, OutputRow> selected;
        while (true) {
            const auto row = reader.next();
            if (row.empty()) break;
            ++rows_read;
            if (row.size() != header.size()) throw std::runtime_error{"Malformed Yahoo stock news row"};
            const auto& report_date = row[date_index];
            if (!valid_date(report_date)) {
                ++invalid_dates;
                continue;
            }
            if (report_date < from_date || report_date > to_date) {
                ++outside_range;
                continue;
            }
            const auto trading_date = calendar.first_session_after(report_date);
            if (trading_date.empty() || trading_date > to_date) {
                ++no_future_session;
                continue;
            }
            const auto& symbol = row[symbol_index];
            const auto sectors = membership.sectors_for(symbol, trading_date);
            if (sectors.empty()) {
                ++membership_rejects;
                continue;
            }
            const auto& uuid = row[uuid_index];
            const auto stable_id = uuid.empty()
                ? sha256(symbol + "|" + report_date + "|" + row[title_index] + "|" + row[link_index])
                : uuid;
            auto text = row[title_index];
            if (!row[article_index].empty()) text += "\n\n" + row[article_index];
            if (text.empty()) continue;
            const auto published_at = report_date + " 00:00:00";
            const auto content_hash = sha256(text);
            for (const auto& sector : sectors) {
                const auto key = stable_id + "|" + symbol + "|" + sector + "|" + trading_date;
                const auto [found, inserted] = selected.emplace(key, OutputRow{
                    .article_id = "yahoo:" + stable_id + ":" + symbol,
                    .published_at_utc = published_at,
                    .trading_date = trading_date,
                    .sector = sector,
                    .symbol = symbol,
                    .text = text,
                    .url = row[link_index],
                    .publisher = row[publisher_index],
                    .content_hash = content_hash
                });
                static_cast<void>(found);
                if (inserted) ++rows_written;
            }
        }

        std::vector<OutputRow> output_rows;
        output_rows.reserve(selected.size());
        for (auto& [_, row] : selected) output_rows.push_back(std::move(row));
        std::ranges::sort(output_rows, [](const auto& left, const auto& right) {
            return std::tie(left.trading_date, left.sector, left.symbol, left.article_id) <
                std::tie(right.trading_date, right.sector, right.symbol, right.article_id);
        });
        const auto output_path = std::filesystem::path{argv[6]};
        if (!output_path.parent_path().empty()) std::filesystem::create_directories(output_path.parent_path());
        std::ofstream output{output_path};
        if (!output) throw std::runtime_error{"Could not write Yahoo article output: " + output_path.string()};
        output << "article_id,original_id,alt_id,first_created,version_created,take_sequence,message_type,pub_status,"
                  "published_at_utc,timestamp_quality,trading_date,sector,symbol,title,summary,url,publisher,content_hash\n";
        for (const auto& row : output_rows) {
            output << csv_escape(row.article_id) << ",,,,,0,,," << row.published_at_utc
                   << ",yahoo_report_date_only," << row.trading_date << ',' << row.sector << ',' << row.symbol << ','
                   << csv_escape(row.text) << ",," << csv_escape(row.url) << ',' << csv_escape(row.publisher) << ','
                   << row.content_hash << '\n';
        }
        std::ofstream manifest{std::string{output_path} + ".manifest.json"};
        if (!manifest) throw std::runtime_error{"Could not write Yahoo article manifest"};
        manifest << "{\n"
                 << "  \"source\": \"bwzheng2010/yahoo-finance-data stock_news.parquet exported to CSV\",\n"
                 << "  \"records_read\": " << rows_read << ",\n"
                 << "  \"invalid_report_dates\": " << invalid_dates << ",\n"
                 << "  \"outside_requested_date_range\": " << outside_range << ",\n"
                 << "  \"no_future_market_session\": " << no_future_session << ",\n"
                 << "  \"membership_rejects\": " << membership_rejects << ",\n"
                 << "  \"rows_written\": " << rows_written << ",\n"
                 << "  \"date_from\": \"" << from_date << "\",\n"
                 << "  \"date_to\": \"" << to_date << "\",\n"
                 << "  \"cutoff_policy\": \"report_date is date-only; every article is assigned to the first market session strictly after the source date\",\n"
                 << "  \"timestamp_quality\": \"yahoo_report_date_only\",\n"
                 << "  \"membership_policy\": \"available_from must be strictly before the assigned trading date; effective interval must contain the trading date\",\n"
                 << "  \"deduplication_policy\": \"stable uuid plus symbol, sector, and assigned session; fallback uses SHA-256 of symbol, date, title, and URL\"\n"
                 << "}\n";
        std::cout << "Yahoo sector articles: " << output_rows.size() << " rows from " << rows_read << " records\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-build-yahoo-sector-articles: " << error.what() << '\n';
        return 1;
    }
}
