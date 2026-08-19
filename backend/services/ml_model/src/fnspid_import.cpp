#include "arrakis/news/xlk_membership.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace {

struct Options final {
    std::filesystem::path input;
    std::filesystem::path holdings;
    std::filesystem::path output{"data/fnspid/normalized/xlk_articles.csv"};
    std::filesystem::path manifest{"data/fnspid/manifests/import.json"};
    std::filesystem::path provenance;
    std::filesystem::path market_history;
    std::string from{"2016-01-01"};
    std::string to{"2023-12-31"};
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
            if (character == '\r' && input.peek() == '\n') {
                input.get(character);
            }
            fields.push_back(field);
            return fields;
        } else {
            field.push_back(character);
        }
    }
    if (quoted) {
        throw std::runtime_error{"FNSPID input ended inside a quoted field"};
    }
    if (!field.empty() || !fields.empty()) {
        fields.push_back(field);
    }
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
    for (const auto byte : digest) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

[[nodiscard]] std::string csv_escape(const std::string& value) {
    if (value.find_first_of(",\"\n\r") == std::string::npos) {
        return value;
    }
    std::string escaped{"\""};
    for (const auto character : value) {
        escaped.push_back(character);
        if (character == '"') {
            escaped.push_back('"');
        }
    }
    escaped.push_back('"');
    return escaped;
}

[[nodiscard]] std::string date_part(const std::string& value) {
    if (value.size() >= 10 && value[4] == '-' && value[7] == '-') {
        return value.substr(0, 10);
    }
    throw std::runtime_error{"Unsupported FNSPID date format: " + value};
}

[[nodiscard]] std::string normalize_utc_timestamp(std::string value) {
    value = trim(std::move(value));
    if (value.ends_with(" UTC")) {
        value.resize(value.size() - 4);
        return value + "Z";
    }
    if (value.ends_with("Z")) return value;
    return value + "Z";
}

[[nodiscard]] std::int64_t parse_utc_seconds(const std::string_view value) {
    if (value.size() < 19) throw std::invalid_argument{"Unsupported FNSPID timestamp: " + std::string{value}};
    std::tm parsed{};
    std::istringstream input{std::string{value.substr(0, 19)}};
    input >> std::get_time(&parsed, "%Y-%m-%d %H:%M:%S");
    if (input.fail()) throw std::invalid_argument{"Unsupported FNSPID timestamp: " + std::string{value}};
    return static_cast<std::int64_t>(timegm(&parsed));
}

[[nodiscard]] std::string date_from_epoch_seconds(const std::int64_t seconds) {
    const auto time = static_cast<time_t>(seconds);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << utc.tm_year + 1900 << '-'
           << std::setw(2) << utc.tm_mon + 1 << '-' << std::setw(2) << utc.tm_mday;
    return output.str();
}

[[nodiscard]] std::string format_utc(const std::int64_t seconds) {
    const auto timestamp = static_cast<time_t>(seconds);
    std::tm utc{};
    gmtime_r(&timestamp, &utc);
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << utc.tm_year + 1900 << '-'
           << std::setw(2) << utc.tm_mon + 1 << '-' << std::setw(2) << utc.tm_mday << ' '
           << std::setw(2) << utc.tm_hour << ':' << std::setw(2) << utc.tm_min << ':'
           << std::setw(2) << utc.tm_sec << 'Z';
    return output.str();
}

[[nodiscard]] std::int64_t cutoff_epoch_for_session(const std::string_view date) {
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

    const bool daylight = (month > 3 && month < 11) ||
        (month == 3 && day >= second_sunday) ||
        (month == 11 && day < first_sunday);

    std::tm cutoff{};
    cutoff.tm_year = year - 1900;
    cutoff.tm_mon = month - 1;
    cutoff.tm_mday = day;
    // 09:20 ET is 13:20 UTC during daylight time and 14:20 UTC otherwise.
    cutoff.tm_hour = daylight ? 13 : 14;
    cutoff.tm_min = 20;
    return static_cast<std::int64_t>(timegm(&cutoff));
}

struct MarketCalendar final {
    struct Session final {
        std::string date;
        std::int64_t cutoff_epoch{};
    };

    std::vector<Session> sessions;

    [[nodiscard]] std::string session_for_publication(const std::int64_t published_epoch) const {
        const auto found = std::ranges::lower_bound(
            sessions, published_epoch, {}, &Session::cutoff_epoch);
        if (found == sessions.end()) return {};
        return found->date;
    }
};

[[nodiscard]] MarketCalendar load_market_calendar(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open market history: " + path.string()};
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error{"Market history is empty: " + path.string()};
    MarketCalendar calendar;
    std::unordered_set<std::string> seen_dates;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::istringstream row{line};
        std::string symbol;
        std::string timestamp;
        if (!std::getline(row, symbol, ',') || !std::getline(row, timestamp, ',')) {
            throw std::runtime_error{"Malformed market history row"};
        }
        const auto date = date_from_epoch_seconds(std::stoll(timestamp));
        if (seen_dates.insert(date).second) {
            calendar.sessions.push_back({date, cutoff_epoch_for_session(date)});
        }
    }
    std::ranges::sort(calendar.sessions, {}, &MarketCalendar::Session::cutoff_epoch);
    if (calendar.sessions.empty()) throw std::runtime_error{"Market history contains no sessions"};
    return calendar;
}

[[nodiscard]] Options parse_options(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (index + 1 >= argc && argument != "--help") {
            throw std::invalid_argument{"Missing value after " + std::string{argument}};
        }
        if (argument == "--input") options.input = argv[++index];
        else if (argument == "--holdings") options.holdings = argv[++index];
        else if (argument == "--output") options.output = argv[++index];
        else if (argument == "--manifest") options.manifest = argv[++index];
        else if (argument == "--provenance") options.provenance = argv[++index];
        else if (argument == "--market-history") options.market_history = argv[++index];
        else if (argument == "--from") options.from = argv[++index];
        else if (argument == "--to") options.to = argv[++index];
        else if (argument == "--help") {
            std::cout << "Usage: arrakis-import-fnspid --input <csv> --holdings <csv> [options]\n"
                      << "  --output <csv>       Normalized output\n"
                      << "  --manifest <json>    Import accounting manifest\n"
                      << "  --provenance <csv>   Row-level PIT assignment ledger\n"
                      << "  --market-history <csv>  Market sessions used for PIT session assignment\n"
                      << "  --from <YYYY-MM-DD>  Inclusive date (default 2016-01-01)\n"
                      << "  --to <YYYY-MM-DD>    Inclusive date (default 2023-12-31)\n";
            std::exit(0);
        } else {
            throw std::invalid_argument{"Unknown option: " + std::string{argument}};
        }
    }
    if (options.input.empty() || options.holdings.empty() || options.market_history.empty()) {
        throw std::invalid_argument{
            "--input, --holdings, and --market-history are required; current holdings are never used as a fallback"};
    }
    return options;
}

// Point-in-time XLK membership.
//
// This used to be a per-symbol interval builder that closed each interval at the
// *next row for the same symbol* and left the last row open on the file's
// 2099-12-31 sentinel. That made every departure permanent: V, MA, PYPL, ADP,
// PAYX, FIS, FISV, GPN, BR, JKHY, IPGP, LDOS, PAYC, VNT and XRXDW all last
// appear in the 2022-12-31 filing (they left XLK in the March 2023 GICS
// reclassification) yet stayed "held" through 2023 and beyond, which is
// survivorship/lookahead contamination of the 2023 test year. It also bridged
// re-entry gaps: CSCO is absent from the four 2021 filings but the old code
// joined 2020-12-31 straight to 2022-03-31.
//
// The correct policy is snapshot-supersedes, and it is already implemented once
// in arrakis::news::XlkMembershipResolver. This importer links that library
// rather than keeping a second copy. The resolver also handles the two known
// physical defects in the shipped CSV explicitly: the file was assembled by
// concatenation, so it repeats its own header at row 955 (the old code ingested
// that as a holding for a symbol literally named "symbol") and repeats the first
// AAPL row at row 956 (identical duplicates are collapsed, conflicting ones
// throw).
[[nodiscard]] arrakis::news::XlkMembershipResolver load_holdings(
    const std::filesystem::path& path
) {
    auto resolver = arrakis::news::XlkMembershipResolver::from_csv(path);
    // Fail loudly if the repeated-header row ever leaks through as a constituent
    // again, and if the file's snapshot structure stops looking like N-PORT
    // quarter ends.
    for (const auto& snapshot : resolver.snapshots()) {
        if (std::ranges::any_of(snapshot.constituents, [](const auto& constituent) {
                return constituent.symbol == "symbol" ||
                       constituent.symbol == "effective_from";
            })) {
            throw std::runtime_error{
                "Holdings history header row was ingested as a constituent in snapshot " +
                snapshot.effective_from};
        }
        if (snapshot.constituents.size() < 30) {
            throw std::runtime_error{
                "Implausibly small XLK snapshot on " + snapshot.effective_from + " (" +
                std::to_string(snapshot.constituents.size()) + " constituents)"};
        }
    }
    std::cout << "Holdings history: " << resolver.snapshots().size() << " quarterly snapshots, "
              << resolver.first_snapshot_date() << " through " << resolver.last_snapshot_date()
              << "; membership before the first snapshot is empty and there is no"
                 " current-holdings fallback\n";
    return resolver;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto holdings = load_holdings(options.holdings);
        const auto market_calendar = load_market_calendar(options.market_history);
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
        if (!output) throw std::runtime_error{"Could not write normalized FNSPID output"};
        output << "article_id,published_at_utc,trading_date,symbol,title,summary,url,publisher,content_hash\n";
        std::ofstream provenance;
        if (!options.provenance.empty()) {
            if (!options.provenance.parent_path().empty()) {
                std::filesystem::create_directories(options.provenance.parent_path());
            }
            provenance.open(options.provenance);
            if (!provenance) throw std::runtime_error{"Could not write FNSPID provenance ledger"};
            provenance << "article_id,source_date,published_at_utc,assigned_trading_date,"
                          "session_cutoff_utc,symbol,governing_membership_snapshot,"
                          "membership_extrapolated,content_hash,identity\n";
        }

        std::size_t rows = 0;
        std::size_t written = 0;
        std::size_t skipped = 0;
        std::size_t duplicates = 0;
        std::size_t missing_timestamp = 0;
        std::size_t membership_rejects = 0;
        // Rows whose trading date is at or after the last available N-PORT
        // snapshot, i.e. resolved against the newest filing carried forward.
        std::size_t extrapolated = 0;
        std::unordered_set<std::string> seen;
        for (auto row = read_record(input); !row.empty(); row = read_record(input)) {
            ++rows;
            if (row.size() <= std::max({date_index, title_index, symbol_index, url_index, publisher_index,
                                        summary_index})) {
                ++skipped;
                continue;
            }
            const auto raw_date = trim(row[date_index]);
            if (raw_date.size() < 10) {
                ++missing_timestamp;
                continue;
            }
            const auto date = date_part(raw_date);
            const auto published_epoch = parse_utc_seconds(raw_date);
            const auto symbol = trim(row[symbol_index]);
            const auto title = trim(row[title_index]);
            if (symbol.empty() || title.empty()) {
                ++skipped;
                continue;
            }
            const auto trading_date = market_calendar.session_for_publication(published_epoch);
            if (trading_date.empty() || trading_date < options.from || trading_date > options.to) {
                ++skipped;
                continue;
            }
            const auto cutoff_epoch = cutoff_epoch_for_session(trading_date);
            if (published_epoch > cutoff_epoch) {
                throw std::runtime_error{
                    "PIT invariant violated: publication is after assigned session cutoff for " +
                    symbol + " on " + trading_date};
            }
            if (!holdings.held_strictly_before(symbol, trading_date)) {
                ++membership_rejects;
                ++skipped;
                continue;
            }
            if (trading_date > holdings.last_snapshot_date()) ++extrapolated;
            const auto summary = trim(row[summary_index]);
            const auto content = lower(title + " " + summary);
            const auto content_hash = sha256(content);
            const auto identity = row[url_index].empty() ? content_hash : trim(row[url_index]);
            if (!seen.insert(identity).second) {
                ++duplicates;
                continue;
            }
            const auto article_id = sha256(symbol + "|" + trading_date + "|" + identity);
            output << article_id << ',' << normalize_utc_timestamp(raw_date) << ',' << trading_date << ',' << symbol << ','
                   << csv_escape(title) << ',' << csv_escape(summary) << ','
                   << csv_escape(trim(row[url_index])) << ',' << csv_escape(trim(row[publisher_index]))
                   << ',' << content_hash << '\n';
            if (provenance.is_open()) {
                const auto snapshot = holdings.governing_snapshot_strictly_before(trading_date);
                provenance << article_id << ',' << date << ',' << normalize_utc_timestamp(raw_date) << ','
                           << trading_date << ',' << format_utc(cutoff_epoch) << ',' << symbol << ','
                           << (snapshot.has_value() ? *snapshot : std::string{}) << ','
                           << (trading_date > holdings.last_snapshot_date() ? "true" : "false") << ','
                           << content_hash << ',' << csv_escape(identity) << '\n';
            }
            ++written;
        }
        std::ofstream manifest{options.manifest};
        if (!manifest) throw std::runtime_error{"Could not write FNSPID manifest"};
        manifest << "{\n  \"source\": \"FNSPID\",\n"
                 << "  \"input\": \"" << options.input.string() << "\",\n"
                 << "  \"date_from\": \"" << options.from << "\",\n"
                 << "  \"date_to\": \"" << options.to << "\",\n"
                 << "  \"rows_read\": " << rows << ",\n"
                 << "  \"rows_written\": " << written << ",\n"
                 << "  \"rows_skipped\": " << skipped << ",\n"
                 << "  \"missing_timestamps\": " << missing_timestamp << ",\n"
                 << "  \"duplicates\": " << duplicates << ",\n"
                 << "  \"membership_rejects\": " << membership_rejects << ",\n"
                 << "  \"holdings_fallback_used\": false,\n"
                 << "  \"holdings_snapshots\": " << holdings.snapshots().size() << ",\n"
                 << "  \"holdings_first_snapshot\": \"" << holdings.first_snapshot_date() << "\",\n"
                 << "  \"holdings_last_snapshot\": \"" << holdings.last_snapshot_date() << "\",\n"
                 << "  \"market_history\": \"" << options.market_history.string() << "\",\n"
                 << "  \"provenance_path\": \"" << options.provenance.string() << "\",\n"
                 << "  \"session_cutoff_policy\": \"first market session whose 09:20 ET publication cutoff is at or after the source UTC timestamp\",\n"
                 << "  \"rows_after_last_snapshot\": " << extrapolated << ",\n"
                 << "  \"holdings_interval_policy\": \"snapshot k is usable only strictly after its "
                    "effective/availability date for this event-time import; the latest prior "
                    "snapshot fully supersedes earlier snapshots, including for symbols that leave "
                    "the fund; effective_to is never used; membership before the first snapshot is "
                    "empty; the final snapshot is carried forward after its date and counted in "
                    "rows_after_last_snapshot\"\n}\n";
        std::cout << "FNSPID import complete: " << written << " rows written, " << skipped
                  << " skipped, " << duplicates << " duplicates, " << extrapolated
                  << " written rows resolved against the carried-forward final snapshot\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FNSPID import failed: " << error.what() << '\n';
        return 1;
    }
}
