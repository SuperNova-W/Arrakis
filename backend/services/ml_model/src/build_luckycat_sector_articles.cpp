#include <boost/json.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::array<std::string_view, 11> kSectors{
    "XLB", "XLC", "XLE", "XLF", "XLI", "XLK", "XLP", "XLRE", "XLU", "XLV", "XLY"};

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

[[nodiscard]] std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) value.pop_back();
    return value;
}

[[nodiscard]] bool valid_date(const std::string_view value) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    for (std::size_t index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U}) {
        if (value[index] < '0' || value[index] > '9') return false;
    }
    return true;
}

[[nodiscard]] bool is_sector(const std::string_view value) {
    return std::ranges::find(kSectors, value) != kSectors.end();
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

    [[nodiscard]] std::string session_for_publication(
        const std::string_view source_date,
        const std::int64_t published_epoch
    ) const {
        const auto daylight = [&]() {
            const auto year = std::stoi(std::string{source_date.substr(0, 4)});
            const auto month = std::stoi(std::string{source_date.substr(5, 2)});
            const auto day = std::stoi(std::string{source_date.substr(8, 2)});
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
        }();
        // 09:20 ET is 13:20 UTC during daylight time and 14:20 UTC otherwise.
        std::tm cutoff_tm{};
        cutoff_tm.tm_year = std::stoi(std::string{source_date.substr(0, 4)}) - 1900;
        cutoff_tm.tm_mon = std::stoi(std::string{source_date.substr(5, 2)}) - 1;
        cutoff_tm.tm_mday = std::stoi(std::string{source_date.substr(8, 2)});
        cutoff_tm.tm_hour = daylight ? 13 : 14;
        cutoff_tm.tm_min = 20;
        const auto cutoff_epoch = static_cast<std::int64_t>(timegm(&cutoff_tm));
        const auto found = published_epoch <= cutoff_epoch
            ? std::ranges::lower_bound(dates, source_date)
            : std::ranges::upper_bound(dates, source_date);
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
    std::ranges::sort(calendar.dates);
    calendar.dates.erase(std::ranges::unique(calendar.dates).begin(), calendar.dates.end());
    if (calendar.dates.empty()) throw std::runtime_error{"Market calendar is empty"};
    return calendar;
}

struct MembershipIndex final {
    std::map<std::string, std::map<std::string, std::unordered_set<std::string>>> snapshots;

    [[nodiscard]] std::vector<std::string> sectors_for(
        const std::string_view symbol,
        const std::string_view date
    ) const {
        const auto snapshot = snapshots.lower_bound(std::string{date});
        if (snapshot == snapshots.begin()) return {};
        const auto& by_sector = std::prev(snapshot)->second;
        std::vector<std::string> result;
        for (const auto sector : kSectors) {
            const auto found_sector = by_sector.find(std::string{sector});
            if (found_sector != by_sector.end() && found_sector->second.contains(std::string{symbol})) {
                result.emplace_back(sector);
            }
        }
        return result;
    }
};

[[nodiscard]] MembershipIndex load_membership(const std::filesystem::path& path) {
    CsvReader reader{path};
    const auto header = reader.next();
    const auto sector = column_index(header, "sector");
    const auto symbol = column_index(header, "symbol");
    const auto available_from = column_index(header, "available_from");
    const auto effective_to = column_index(header, "effective_to");
    MembershipIndex result;
    while (true) {
        const auto row = reader.next();
        if (row.empty()) break;
        if (row.size() != header.size()) throw std::runtime_error{"Malformed sector holdings row"};
        if (!is_sector(row[sector]) || row[symbol].empty()) continue;
        if (row[effective_to] != "2099-12-31") {
            throw std::runtime_error{"Sector holdings effective_to must be the sentinel"};
        }
        result.snapshots[row[available_from]][row[sector]].insert(row[symbol]);
    }
    if (result.snapshots.empty()) throw std::runtime_error{"Sector holdings history is empty"};
    return result;
}

[[nodiscard]] std::int64_t parse_utc_seconds(const std::string_view value) {
    if (value.size() < 19) throw std::invalid_argument{"Publication timestamp is too short"};
    std::tm parsed{};
    std::istringstream input{std::string{value.substr(0, 19)}};
    input >> std::get_time(&parsed, "%Y-%m-%d %H:%M:%S");
    if (input.fail()) throw std::invalid_argument{"Invalid publication timestamp"};
    return static_cast<std::int64_t>(timegm(&parsed));
}

[[nodiscard]] std::string format_utc(const std::int64_t seconds) {
    const auto timestamp = static_cast<std::time_t>(seconds);
    std::tm utc{};
    gmtime_r(&timestamp, &utc);
    std::array<char, 21> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d %H:%M:%SZ", &utc) == 0) {
        throw std::runtime_error{"Could not format publication timestamp"};
    }
    return buffer.data();
}

[[nodiscard]] std::string string_value(const boost::json::object& object, const std::string_view key) {
    const auto* value = object.if_contains(key);
    return value != nullptr && value->is_string() ? std::string{value->as_string()} : std::string{};
}

[[nodiscard]] std::vector<std::string> string_array(
    const boost::json::object& object,
    const std::string_view key
) {
    std::vector<std::string> result;
    const auto* value = object.if_contains(key);
    if (value == nullptr || !value->is_array()) return result;
    for (const auto& item : value->as_array()) {
        if (item.is_string() && !item.as_string().empty()) result.emplace_back(item.as_string());
    }
    return result;
}

[[nodiscard]] std::string read_source(const std::filesystem::path& path) {
    if (path.extension() != ".xz") {
        std::ifstream input{path, std::ios::binary};
        if (!input) throw std::runtime_error{"Could not open JSON: " + path.string()};
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }
    const auto shell_path = path.string();
    if (shell_path.find('\'') != std::string::npos) throw std::runtime_error{"Input path contains an unsupported quote"};
    const auto command = "xz -dc -- '" + shell_path + "'";
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) throw std::runtime_error{"Could not open xz stream"};
    std::string result;
    std::array<char, 1U << 16U> buffer{};
    while (true) {
        const auto count = std::fread(buffer.data(), 1, buffer.size(), pipe);
        result.append(buffer.data(), count);
        if (count < buffer.size()) break;
    }
    const auto status = pclose(pipe);
    if (status != 0) throw std::runtime_error{"xz decompression failed for " + path.string()};
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

struct Stats final {
    std::size_t records_read{};
    std::size_t invalid_dates{};
    std::size_t outside_range{};
    std::size_t no_future_session{};
    std::size_t no_ticker{};
    std::size_t membership_rejects{};
    std::size_t duplicates{};
    std::size_t rows_written{};
    std::set<std::string> dates;
    std::set<std::string> sectors;
    std::map<std::string, std::size_t> rows_by_sector;
};

void process_file(
    const std::filesystem::path& path,
    const std::string_view from_date,
    const std::string_view to_date,
    const Calendar& calendar,
    const MembershipIndex& membership,
    std::unordered_map<std::string, OutputRow>& selected,
    Stats& stats,
    const std::string_view requested_sector
) {
    const auto source = read_source(path);
    boost::system::error_code parse_error;
    boost::json::parse_options parse_options;
    parse_options.allow_invalid_utf8 = true;
    parse_options.allow_infinity_and_nan = true;
    const auto root = boost::json::parse(source, parse_error, {}, parse_options);
    if (parse_error) {
        throw std::runtime_error{"Invalid JSON in " + path.string() + ": " + parse_error.message()};
    }
    if (!root.is_array()) throw std::runtime_error{"Input is not a JSON array: " + path.string()};
    for (const auto& item : root.as_array()) {
        ++stats.records_read;
        if (!item.is_object()) continue;
        const auto& object = item.as_object();
        const auto raw_date = string_value(object, "date_publish");
        const auto raw_download = string_value(object, "date_download");
        if (raw_date.size() < 10 || !valid_date(raw_date.substr(0, 10))) {
            ++stats.invalid_dates;
            continue;
        }
        std::int64_t publication_epoch = 0;
        std::int64_t download_epoch = 0;
        try {
            publication_epoch = parse_utc_seconds(raw_date);
            download_epoch = raw_download.empty() ? publication_epoch : parse_utc_seconds(raw_download);
        } catch (const std::exception&) {
            ++stats.invalid_dates;
            continue;
        }
        const auto availability_epoch = std::max(publication_epoch, download_epoch);
        const auto availability_date = format_utc(availability_epoch).substr(0, 10);
        if (availability_date < from_date || availability_date > to_date) {
            ++stats.outside_range;
            continue;
        }
        const auto trading_date = calendar.session_for_publication(availability_date, availability_epoch);
        if (trading_date.empty() || trading_date > to_date) {
            ++stats.no_future_session;
            continue;
        }
        const auto title = trim(string_value(object, "title"));
        const auto body = trim(string_value(object, "maintext"));
        const auto text = body.empty() ? title : title + "\n\n" + body;
        if (text.empty()) continue;
        auto symbols = string_array(object, "mentioned_companies");
        std::ranges::sort(symbols);
        symbols.erase(std::ranges::unique(symbols).begin(), symbols.end());
        if (symbols.empty()) {
            ++stats.no_ticker;
            continue;
        }
        const auto url = string_value(object, "url");
        const auto publisher = string_value(object, "news_outlet").empty()
            ? string_value(object, "source_domain") : string_value(object, "news_outlet");
        const auto content_hash = sha256(text);
        const auto stable_id = sha256(url + "|" + raw_date + "|" + title);
        bool wrote_any = false;
        for (const auto& symbol : symbols) {
            const auto sectors = membership.sectors_for(symbol, trading_date);
            if (sectors.empty()) continue;
            for (const auto& sector : sectors) {
                if (!requested_sector.empty() && sector != requested_sector) continue;
                const auto key = sector + "|" + content_hash + "|" + trading_date;
                const auto [_, inserted] = selected.emplace(key, OutputRow{
                    .article_id = "luckycat:" + stable_id + ":" + symbol,
                    .published_at_utc = format_utc(availability_epoch),
                    .trading_date = trading_date,
                    .sector = sector,
                    .symbol = symbol,
                    .text = text,
                    .url = url,
                    .publisher = publisher,
                    .content_hash = content_hash});
                if (inserted) {
                    ++stats.rows_written;
                    ++stats.rows_by_sector[sector];
                    wrote_any = true;
                } else {
                    ++stats.duplicates;
                }
            }
        }
        if (!wrote_any) ++stats.membership_rejects;
        else {
            stats.dates.insert(trading_date);
            for (const auto& sector : kSectors) {
                if (stats.rows_by_sector.contains(std::string{sector})) stats.sectors.insert(std::string{sector});
            }
        }
    }
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 7 && argc != 8) {
            std::cout << "Usage: arrakis-build-luckycat-sector-articles <json-dir> <sector-holdings-history.csv> "
                         "<market-calendar.csv> <from-date> <to-date> <output.csv> [sector]\n";
            return 0;
        }
        const auto input_dir = std::filesystem::path{argv[1]};
        const auto from_date = std::string{argv[4]};
        const auto to_date = std::string{argv[5]};
        if (!valid_date(from_date) || !valid_date(to_date) || from_date > to_date) {
            throw std::invalid_argument{"Invalid date range"};
        }
        const auto requested_sector = argc == 8 ? std::string{argv[7]} : std::string{};
        if (!requested_sector.empty() && !is_sector(requested_sector)) {
            throw std::invalid_argument{"Unknown sector filter: " + requested_sector};
        }
        const auto calendar = load_calendar(argv[3]);
        const auto membership = load_membership(argv[2]);
        std::unordered_map<std::string, OutputRow> selected;
        Stats stats;
        for (int year = 2017; year <= 2023; ++year) {
            const auto path = input_dir / (std::to_string(year) + "_processed.json.xz");
            if (std::filesystem::exists(path)) process_file(
                path, from_date, to_date, calendar, membership, selected, stats, requested_sector);
        }
        if (selected.empty()) throw std::runtime_error{"No eligible articles found"};
        std::vector<OutputRow> rows;
        rows.reserve(selected.size());
        for (auto& [_, row] : selected) rows.push_back(std::move(row));
        std::ranges::sort(rows, [](const auto& left, const auto& right) {
            return std::tie(left.trading_date, left.sector, left.article_id) <
                   std::tie(right.trading_date, right.sector, right.article_id);
        });
        const auto output_path = std::filesystem::path{argv[6]};
        if (!output_path.parent_path().empty()) std::filesystem::create_directories(output_path.parent_path());
        std::ofstream output{output_path};
        if (!output) throw std::runtime_error{"Could not write output: " + output_path.string()};
        output << "article_id,original_id,alt_id,first_created,version_created,take_sequence,message_type,pub_status,"
                  "published_at_utc,timestamp_quality,trading_date,sector,symbol,title,summary,url,publisher,content_hash\n";
        for (const auto& row : rows) {
            output << csv_escape(row.article_id) << ",,,,,0,,," << row.published_at_utc
                   << ",yahoo_date_publish_utc_preopen_verified," << row.trading_date << ',' << row.sector << ','
                   << row.symbol << ',' << csv_escape(row.text) << ",," << csv_escape(row.url) << ','
                   << csv_escape(row.publisher) << ',' << row.content_hash << '\n';
        }
        std::ofstream manifest{std::string{output_path} + ".manifest.json"};
        if (!manifest) throw std::runtime_error{"Could not write import manifest"};
        manifest << "{\n"
                 << "  \"source\": \"luckycat37/financial-news-dataset\",\n"
                 << "  \"license\": \"cc-by-nc-sa-4.0\",\n"
                 << "  \"records_read\": " << stats.records_read << ",\n"
                 << "  \"invalid_dates\": " << stats.invalid_dates << ",\n"
                 << "  \"outside_requested_date_range\": " << stats.outside_range << ",\n"
                 << "  \"no_future_market_session\": " << stats.no_future_session << ",\n"
                 << "  \"no_mentioned_ticker\": " << stats.no_ticker << ",\n"
                 << "  \"membership_rejects\": " << stats.membership_rejects << ",\n"
                 << "  \"duplicates\": " << stats.duplicates << ",\n"
                 << "  \"rows_written\": " << rows.size() << ",\n"
                 << "  \"distinct_trading_dates\": " << stats.dates.size() << ",\n"
                 << "  \"distinct_sectors\": " << stats.sectors.size() << ",\n"
                 << "  \"from_date\": \"" << from_date << "\",\n"
                 << "  \"to_date\": \"" << to_date << "\",\n"
                 << "  \"timestamp_policy\": \"availability is max(date_publish, date_download), both interpreted as UTC; availability at or before 09:20 ET uses that session, later availability uses the next session; weekends and holidays use the next available session\",\n"
                 << "  \"ticker_policy\": \"mentioned_companies only; precomputed prices and sentiment fields are not read\",\n"
                 << "  \"sector_filter\": \"" << requested_sector << "\",\n"
                 << "  \"membership_policy\": \"available_from must be strictly before the assigned trading date and the effective interval must contain it\",\n"
                 << "  \"deduplication_policy\": \"one canonical row per sector, content_hash, and assigned trading session\",\n"
                 << "  \"rows_by_sector\": {\n";
        std::size_t index = 0;
        for (const auto sector : kSectors) {
            const auto found = stats.rows_by_sector.find(std::string{sector});
            manifest << "    \"" << sector << "\": " << (found == stats.rows_by_sector.end() ? 0 : found->second)
                     << (++index == kSectors.size() ? "\n" : ",\n");
        }
        manifest << "  }\n}\n";
        std::cout << "Luckycat sector articles: " << rows.size() << " rows from " << stats.records_read
                  << " records, " << stats.duplicates << " duplicates\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-build-luckycat-sector-articles: " << error.what() << '\n';
        return 1;
    }
}
