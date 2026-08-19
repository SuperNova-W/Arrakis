#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
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
#include <ctime>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::string_view kSentinelEffectiveTo{"2099-12-31"};
constexpr std::array<std::string_view, 11> kSectors{
    "XLB", "XLC", "XLE", "XLF", "XLI", "XLK", "XLP", "XLRE", "XLU", "XLV", "XLY"};

struct Options final {
    std::filesystem::path input;
    std::filesystem::path holdings;
    std::filesystem::path market_calendar{"data/history/SPY.csv"};
    std::filesystem::path output{"data/fnspid/normalized/sector_articles.csv"};
    std::filesystem::path manifest{"data/fnspid/manifests/sector_import.json"};
    std::string timestamp_policy{"conservative"};
    std::string from{"2019-01-01"};
    std::string to{"2023-12-31"};
};

struct NormalizedRow final {
    std::string article_id;
    std::string published_at_raw;
    std::string published_at_utc;
    std::string source_calendar_date;
    std::string trading_date;
    std::string sector;
    std::string symbol;
    std::string title;
    std::string summary;
    std::string url;
    std::string publisher;
    std::string content_hash;
    std::string timestamp_quality;
    bool extrapolated{};
};

[[nodiscard]] std::vector<std::string> read_record(std::istream& input) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    char character = 0;
    while (input.get(character)) {
        if (character == '"') {
            if (quoted && input.peek() == '"') {
                input.get(character);
                field.push_back('"');
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
    if (quoted) throw std::runtime_error{"FNSPID input ended inside a quoted field"};
    if (!field.empty() || !fields.empty()) fields.push_back(field);
    return fields;
}

[[nodiscard]] std::vector<std::string> split_csv_line(const std::string& line) {
    std::istringstream input{line};
    std::vector<std::string> fields;
    std::string field;
    while (std::getline(input, field, ',')) fields.push_back(field);
    if (!line.empty() && line.back() == ',') fields.emplace_back();
    return fields;
}

[[nodiscard]] std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

[[nodiscard]] std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] std::string sha256(const std::string_view value) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
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

[[nodiscard]] std::string date_part(const std::string& value) {
    if (value.size() >= 10 && value[4] == '-' && value[7] == '-') return value.substr(0, 10);
    throw std::runtime_error{"Unsupported FNSPID date format: " + value};
}

[[nodiscard]] std::string normalize_utc_timestamp(std::string value) {
    value = trim(std::move(value));
    if (value.ends_with(" UTC")) {
        value.resize(value.size() - 4);
        return value + "Z";
    }
    if (value.ends_with('Z')) return value;
    return value + "Z";
}

[[nodiscard]] std::string date_from_epoch(const std::int64_t epoch_seconds) {
    const auto timestamp = static_cast<std::time_t>(epoch_seconds);
    std::tm utc{};
    if (gmtime_r(&timestamp, &utc) == nullptr) throw std::runtime_error{"Invalid market timestamp"};
    std::array<char, 11> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d", &utc) == 0) {
        throw std::runtime_error{"Could not format market date"};
    }
    return buffer.data();
}

[[nodiscard]] std::int64_t parse_utc_seconds(std::string value) {
    value = trim(std::move(value));
    if (value.ends_with(" UTC")) value.resize(value.size() - 4);
    if (value.ends_with('Z')) value.pop_back();
    if (value.size() < 19) throw std::invalid_argument{"Timestamp has no clock component"};
    std::tm parsed{};
    std::istringstream input{value.substr(0, 19)};
    input >> std::get_time(&parsed, "%Y-%m-%d %H:%M:%S");
    if (input.fail()) throw std::invalid_argument{"Invalid timestamp: " + value};
    return static_cast<std::int64_t>(timegm(&parsed));
}

[[nodiscard]] bool is_us_daylight(const std::string_view date) {
    const auto year = std::stoi(std::string{date.substr(0, 4)});
    const auto month = std::stoi(std::string{date.substr(5, 2)});
    const auto day = std::stoi(std::string{date.substr(8, 2)});
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
    return (month > 3 && month < 11) ||
           (month == 3 && day >= second_sunday) ||
           (month == 11 && day < first_sunday);
}

[[nodiscard]] std::int64_t preopen_epoch(const std::string_view date) {
    std::tm preopen{};
    preopen.tm_year = std::stoi(std::string{date.substr(0, 4)}) - 1900;
    preopen.tm_mon = std::stoi(std::string{date.substr(5, 2)}) - 1;
    preopen.tm_mday = std::stoi(std::string{date.substr(8, 2)});
    preopen.tm_hour = is_us_daylight(date) ? 13 : 14;
    preopen.tm_min = 20;
    return static_cast<std::int64_t>(timegm(&preopen));
}

[[nodiscard]] std::string format_utc_timestamp(const std::int64_t epoch_seconds) {
    const auto timestamp = static_cast<std::time_t>(epoch_seconds);
    std::tm utc{};
    if (gmtime_r(&timestamp, &utc) == nullptr) throw std::runtime_error{"Invalid repaired timestamp"};
    std::array<char, 21> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d %H:%M:%SZ", &utc) == 0) {
        throw std::runtime_error{"Could not format repaired timestamp"};
    }
    return buffer.data();
}

class TradingCalendar final {
public:
    static TradingCalendar from_csv(const std::filesystem::path& path) {
        std::ifstream input{path};
        if (!input) throw std::runtime_error{"Could not open market calendar: " + path.string()};
        std::string line;
        if (!std::getline(input, line)) throw std::runtime_error{"Market calendar is empty"};
        TradingCalendar result;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            const auto fields = split_csv_line(line);
            if (fields.size() < 2) throw std::runtime_error{"Malformed market calendar row"};
            const auto date = date_from_epoch(std::stoll(fields[1]));
            if (result.dates_.empty() || result.dates_.back() != date) result.dates_.push_back(date);
        }
        if (result.dates_.size() < 100) throw std::runtime_error{"Market calendar has too few sessions"};
        return result;
    }

    // FNSPID's released preprocessing labels timestamps UTC, but its documented
    // timezone conversion is not independently trustworthy.  Treat every source
    // clock value as unverified and delay it to the first exchange session after
    // the source calendar date.  This is conservative and prevents same-day
    // publication leakage until a source-specific timestamp can be verified.
    [[nodiscard]] std::string session_for_unverified(
        const std::string_view source_date,
        bool& shifted
    ) const {
        const auto found = std::ranges::upper_bound(dates_, source_date);
        if (found == dates_.end()) throw std::runtime_error{"Publication is after the market calendar"};
        shifted = true;
        return *found;
    }

    [[nodiscard]] std::string session_for_verified_preopen(
        const std::string_view source_date,
        const std::int64_t published_epoch,
        bool& shifted
    ) const {
        auto found = std::ranges::lower_bound(dates_, source_date);
        while (found != dates_.end()) {
            if (published_epoch <= preopen_epoch(*found)) {
                shifted = *found != source_date;
                return *found;
            }
            ++found;
        }
        throw std::runtime_error{"Publication is after the market calendar"};
    }

    [[nodiscard]] const std::string& first_date() const { return dates_.front(); }
    [[nodiscard]] const std::string& last_date() const { return dates_.back(); }

private:
    std::vector<std::string> dates_;
};

struct TimestampAssignment final {
    std::string session_date;
    std::string published_at_utc;
    std::string quality;
    bool shifted{};
};

[[nodiscard]] TimestampAssignment assign_timestamp(
    const TradingCalendar& calendar,
    const std::string& raw_value,
    const std::string& source_date,
    const std::string_view policy
) {
    const auto trimmed = trim(raw_value);
    const bool has_clock = trimmed.size() >= 19 && trimmed[10] == ' ';
    const bool date_only = !has_clock || trimmed.substr(11, 8) == "00:00:00";
    if (policy != "fnspid-inverse-candidate" || date_only) {
        bool shifted = false;
        return TimestampAssignment{
            .session_date = calendar.session_for_unverified(source_date, shifted),
            .published_at_utc = normalize_utc_timestamp(trimmed),
            .quality = date_only ? "fnspid_date_only" : "fnspid_utc_unverified",
            .shifted = shifted,
        };
    }

    const auto raw_epoch = parse_utc_seconds(trimmed);
    // The released FNSPID preprocessing example subtracts the EST/EDT offset
    // instead of converting local time to UTC. Reverse that transform only in
    // this explicitly named candidate policy.
    const auto repaired_epoch = raw_epoch + (is_us_daylight(source_date) ? 8 : 10) * 3600;
    bool shifted = false;
    return TimestampAssignment{
        .session_date = calendar.session_for_verified_preopen(source_date, repaired_epoch, shifted),
        .published_at_utc = format_utc_timestamp(repaired_epoch),
        .quality = "fnspid_inverse_est_edt_candidate",
        .shifted = shifted,
    };
}

[[nodiscard]] bool is_sector(const std::string_view value) {
    return std::ranges::find(kSectors, value) != kSectors.end();
}

class SectorMembership final {
public:
    static SectorMembership from_csv(const std::filesystem::path& path) {
        std::ifstream input{path};
        if (!input) throw std::runtime_error{"Could not open sector holdings: " + path.string()};
        std::string line;
        if (!std::getline(input, line)) throw std::runtime_error{"Sector holdings are empty"};
        const auto header = split_csv_line(line);
        const std::vector<std::string> expected{
            "sector", "symbol", "effective_from", "available_from", "effective_to", "weight", "source"};
        if (header.size() < expected.size() || !std::ranges::equal(expected, header | std::views::take(expected.size()))) {
            throw std::runtime_error{
                "Sector holdings must start with sector,symbol,effective_from,available_from,effective_to,weight,source"};
        }

        SectorMembership result;
        while (std::getline(input, line)) {
            if (trim(line).empty()) continue;
            const auto fields = split_csv_line(line);
            if (fields.size() < 7) throw std::runtime_error{"Malformed sector holdings row"};
            const auto& sector = fields[0];
            const auto& symbol = fields[1];
            const auto& effective_from = fields[2];
            const auto& available_from = fields[3];
            const auto& effective_to = fields[4];
            if (!is_sector(sector) || symbol.empty() || effective_from.size() != 10 || available_from.size() != 10) {
                throw std::runtime_error{"Invalid sector holdings identity"};
            }
            if (effective_to != kSentinelEffectiveTo) {
                throw std::runtime_error{"Sector holdings effective_to must be the sentinel"};
            }
            try {
                const auto weight = std::stod(fields[5]);
                if (!(weight >= 0.0)) throw std::runtime_error{"negative weight"};
            } catch (const std::exception&) {
                throw std::runtime_error{"Invalid sector holdings weight"};
            }
            // A filing is treated as available only after its SEC filing date.
            // lower_bound(date) in held_sectors therefore excludes same-day
            // articles and avoids using a quarter-end snapshot before it was
            // public.
            result.snapshots_[available_from][sector].insert(symbol);
        }
        if (result.snapshots_.empty()) throw std::runtime_error{"Sector holdings have no snapshots"};
        for (const auto& sector : kSectors) {
            if (!result.has_sector(sector)) {
                throw std::runtime_error{"Sector holdings are missing configured sector " + std::string{sector}};
            }
        }
        return result;
    }

    [[nodiscard]] std::vector<std::string> held_sectors(
        const std::string_view symbol,
        const std::string_view date
    ) const {
        const auto snapshot = snapshots_.lower_bound(std::string{date});
        if (snapshot == snapshots_.begin()) return {};
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

    [[nodiscard]] std::size_t snapshot_count() const { return snapshots_.size(); }
    [[nodiscard]] const std::string& first_snapshot_date() const { return snapshots_.begin()->first; }
    [[nodiscard]] const std::string& last_snapshot_date() const { return snapshots_.rbegin()->first; }

    [[nodiscard]] bool is_extrapolated_forward(
        const std::string_view date,
        const std::string_view sector
    ) const {
        const auto snapshot = snapshots_.find(std::string{last_snapshot_date()});
        return date > last_snapshot_date() && snapshot->second.contains(std::string{sector});
    }

private:
    [[nodiscard]] bool has_sector(const std::string_view sector) const {
        return std::ranges::any_of(snapshots_, [sector](const auto& item) {
            return item.second.contains(std::string{sector});
        });
    }

    std::map<std::string, std::map<std::string, std::unordered_set<std::string>>> snapshots_;
};

[[nodiscard]] Options parse_options(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (index + 1 >= argc && argument != "--help") {
            throw std::invalid_argument{"Missing value after " + std::string{argument}};
        }
        if (argument == "--input") options.input = argv[++index];
        else if (argument == "--holdings") options.holdings = argv[++index];
        else if (argument == "--market-calendar") options.market_calendar = argv[++index];
        else if (argument == "--output") options.output = argv[++index];
        else if (argument == "--manifest") options.manifest = argv[++index];
        else if (argument == "--timestamp-policy") options.timestamp_policy = argv[++index];
        else if (argument == "--from") options.from = argv[++index];
        else if (argument == "--to") options.to = argv[++index];
        else if (argument == "--help") {
            std::cout << "Usage: arrakis-import-fnspid-sectors --input <csv> --holdings <csv> [options]\n"
                      << "  --output <csv>       Normalized sector output\n"
                      << "  --manifest <json>    Import accounting manifest\n"
                      << "  --market-calendar <csv>  Exchange-session calendar (default data/history/SPY.csv)\n"
                      << "  --timestamp-policy <name> conservative (default) or fnspid-inverse-candidate\n"
                      << "  --from <YYYY-MM-DD>  Inclusive date (default 2019-01-01)\n"
                      << "  --to <YYYY-MM-DD>    Inclusive date (default 2023-12-31)\n";
            std::exit(0);
        } else {
            throw std::invalid_argument{"Unknown option: " + std::string{argument}};
        }
    }
    if (options.input.empty() || options.holdings.empty()) {
        throw std::invalid_argument{"--input and --holdings are required"};
    }
    if (options.timestamp_policy != "conservative" &&
        options.timestamp_policy != "fnspid-inverse-candidate") {
        throw std::invalid_argument{"Unsupported timestamp policy: " + options.timestamp_policy};
    }
    return options;
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto holdings = SectorMembership::from_csv(options.holdings);
        const auto calendar = TradingCalendar::from_csv(options.market_calendar);
        std::ifstream input{options.input};
        if (!input) throw std::runtime_error{"Could not open FNSPID CSV: " + options.input.string()};
        const auto header = read_record(input);
        const auto index_of = [&](const std::string& name) -> std::size_t {
            const auto found = std::ranges::find(header, name);
            if (found == header.end()) throw std::runtime_error{"FNSPID is missing column: " + name};
            return static_cast<std::size_t>(std::distance(header.begin(), found));
        };
        const auto date_index = std::ranges::find(header, "Date string") != header.end()
                                    ? static_cast<std::size_t>(std::distance(
                                          header.begin(), std::ranges::find(header, "Date string")))
                                    : index_of("Date");
        const auto title_index = index_of("Article_title");
        const auto symbol_index = index_of("Stock_symbol");
        const auto url_index = index_of("Url");
        const auto publisher_index = index_of("Publisher");
        const auto summary_index = std::ranges::find(header, "Article") == header.end()
                                       ? title_index
                                       : static_cast<std::size_t>(std::distance(
                                             header.begin(), std::ranges::find(header, "Article")));

        std::filesystem::create_directories(options.output.parent_path());
        std::filesystem::create_directories(options.manifest.parent_path());
        std::ofstream output{options.output};
        if (!output) throw std::runtime_error{"Could not write normalized sector output"};
        std::size_t rows = 0;
        std::size_t skipped = 0;
        std::size_t duplicates = 0;
        std::size_t missing_timestamp = 0;
        std::size_t extrapolated = 0;
        std::size_t session_shifted = 0;
        std::unordered_map<std::string, NormalizedRow> canonical_rows;
        std::map<std::string, std::size_t> written_by_sector;
        for (auto row = read_record(input); !row.empty(); row = read_record(input)) {
            ++rows;
            if (row.size() <= std::max({date_index, title_index, symbol_index, url_index, publisher_index, summary_index})) {
                ++skipped;
                continue;
            }
            const auto raw_date = trim(row[date_index]);
            if (raw_date.size() < 10) {
                ++missing_timestamp;
                continue;
            }
            const auto source_date = date_part(raw_date);
            const auto timestamp = assign_timestamp(
                calendar, raw_date, source_date, options.timestamp_policy
            );
            const auto& date = timestamp.session_date;
            if (date < options.from || date > options.to) continue;
            if (timestamp.shifted) ++session_shifted;
            const auto symbol = trim(row[symbol_index]);
            const auto title = trim(row[title_index]);
            const auto sectors = holdings.held_sectors(symbol, source_date);
            if (symbol.empty() || title.empty() || sectors.empty()) {
                ++skipped;
                continue;
            }
            const auto summary = trim(row[summary_index]);
            const auto content = lower(title + " " + summary);
            const auto content_hash = sha256(content);
            for (const auto& sector : sectors) {
                const auto dedup_key = sector + "|" + content_hash;
                const auto extrapolated_row = holdings.is_extrapolated_forward(source_date, sector);
                NormalizedRow candidate{
                    .article_id = sha256(sector + "|" + symbol + "|" + date + "|" + content_hash),
                    .published_at_raw = raw_date,
                    .published_at_utc = timestamp.published_at_utc,
                    .source_calendar_date = source_date,
                    .trading_date = date,
                    .sector = sector,
                    .symbol = symbol,
                    .title = title,
                    .summary = summary,
                    .url = trim(row[url_index]),
                    .publisher = trim(row[publisher_index]),
                    .content_hash = content_hash,
                    .timestamp_quality = timestamp.quality,
                    .extrapolated = extrapolated_row,
                };
                const auto found = canonical_rows.find(dedup_key);
                if (found == canonical_rows.end()) {
                    canonical_rows.emplace(dedup_key, std::move(candidate));
                } else {
                    ++duplicates;
                    const auto& current = found->second;
                    const auto candidate_key = std::tie(candidate.trading_date, candidate.source_calendar_date,
                                                         candidate.article_id);
                    const auto current_key = std::tie(current.trading_date, current.source_calendar_date,
                                                       current.article_id);
                    if (candidate_key < current_key) found->second = std::move(candidate);
                }
            }
        }

        std::vector<NormalizedRow> normalized_rows;
        normalized_rows.reserve(canonical_rows.size());
        for (auto& [key, row] : canonical_rows) {
            static_cast<void>(key);
            normalized_rows.push_back(std::move(row));
        }
        std::ranges::sort(normalized_rows, [](const auto& left, const auto& right) {
            return std::tie(left.trading_date, left.sector, left.article_id) <
                   std::tie(right.trading_date, right.sector, right.article_id);
        });
        std::map<std::string, std::size_t> timestamp_quality_counts;
        for (const auto& row : normalized_rows) ++timestamp_quality_counts[row.timestamp_quality];
        output << "article_id,published_at_raw,published_at_utc,timestamp_quality,source_calendar_date,"
                  "trading_date,sector,symbol,title,summary,url,publisher,content_hash\n";
        for (const auto& row : normalized_rows) {
            output << row.article_id << ',' << csv_escape(row.published_at_raw) << ','
                   << row.published_at_utc << ',' << row.timestamp_quality << ',' << row.source_calendar_date << ','
                   << row.trading_date << ',' << row.sector << ',' << row.symbol << ','
                   << csv_escape(row.title) << ',' << csv_escape(row.summary) << ',' << csv_escape(row.url)
                   << ',' << csv_escape(row.publisher) << ',' << row.content_hash << '\n';
            ++written_by_sector[row.sector];
            if (row.extrapolated) ++extrapolated;
        }
        const auto written = normalized_rows.size();

        std::ofstream manifest{options.manifest};
        if (!manifest) throw std::runtime_error{"Could not write sector FNSPID manifest"};
        manifest << "{\n  \"source\": \"FNSPID\",\n"
                 << "  \"input\": \"" << options.input.string() << "\",\n"
                 << "  \"date_from\": \"" << options.from << "\",\n"
                 << "  \"date_to\": \"" << options.to << "\",\n"
                 << "  \"rows_read\": " << rows << ",\n"
                 << "  \"rows_written\": " << written << ",\n"
                 << "  \"rows_skipped\": " << skipped << ",\n"
                 << "  \"missing_timestamps\": " << missing_timestamp << ",\n"
                 << "  \"session_shifted_rows\": " << session_shifted << ",\n"
                 << "  \"timestamp_quality_counts\": {";
        std::size_t quality_index = 0;
        for (const auto& [quality, count] : timestamp_quality_counts) {
            manifest << "\"" << quality << "\": " << count
                     << (++quality_index == timestamp_quality_counts.size() ? "},\n" : ", ");
        }
        manifest
                 << "  \"market_calendar\": \"" << options.market_calendar.string() << "\",\n"
                 << "  \"market_calendar_first_session\": \"" << calendar.first_date() << "\",\n"
                 << "  \"market_calendar_last_session\": \"" << calendar.last_date() << "\",\n"
                 << "  \"duplicates\": " << duplicates << ",\n"
                 << "  \"holdings_fallback_used\": false,\n"
                 << "  \"holdings_snapshots\": " << holdings.snapshot_count() << ",\n"
                 << "  \"holdings_first_snapshot\": \"" << holdings.first_snapshot_date() << "\",\n"
                 << "  \"holdings_last_snapshot\": \"" << holdings.last_snapshot_date() << "\",\n"
                 << "  \"rows_after_last_snapshot\": " << extrapolated << ",\n"
                 << "  \"timestamp_policy\": \"" << options.timestamp_policy << "\",\n"
                 << "  \"session_assignment_policy\": \""
                 << (options.timestamp_policy == "conservative"
                         ? "unverified FNSPID rows use the first SPY session strictly after source_calendar_date"
                         : "non-midnight FNSPID rows use the inverse EST/EDT candidate transform and first session at or after the 09:20 ET pre-open cutoff; date-only rows remain conservatively delayed")
                 << "\",\n"
                 << "  \"deduplication_policy\": \"one canonical row per sector and content_hash; earliest assigned trading session wins, then source date and article id\",\n"
                 << "  \"holdings_interval_policy\": \"snapshot k is usable only for articles strictly after its SEC filing available_from date; same-day articles are excluded; membership before the first available_from is empty; the final snapshot is carried forward\",\n"
                 << "  \"rows_by_sector\": {\n";
        std::size_t index = 0;
        for (const auto& sector : kSectors) {
            const auto found = written_by_sector.find(std::string{sector});
            manifest << "    \"" << sector << "\": " << (found == written_by_sector.end() ? 0 : found->second)
                     << (++index == kSectors.size() ? "\n" : ",\n");
        }
        manifest << "  }\n}\n";
        std::cout << "Sector FNSPID import complete: " << written << " rows written, " << skipped
                  << " skipped, " << duplicates << " duplicates\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Sector FNSPID import failed: " << error.what() << '\n';
        return 1;
    }
}
