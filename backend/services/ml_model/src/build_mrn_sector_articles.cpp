#include <boost/json.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
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
#include <unordered_set>
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

[[nodiscard]] std::string lower_copy(std::string value) {
    for (auto& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
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

struct ParsedTimestamp final {
    std::int64_t epoch_seconds{};
    std::string date;
    std::string normalized;
};

[[nodiscard]] ParsedTimestamp parse_timestamp(const std::string_view raw) {
    if (raw.size() < 20 || raw[4] != '-' || raw[7] != '-' || raw[10] != 'T' || raw[13] != ':' ||
        raw[16] != ':') {
        throw std::runtime_error{"MRN timestamp is not ISO-8601 UTC: " + std::string{raw}};
    }
    const auto year = std::stoi(std::string{raw.substr(0, 4)});
    const auto month = static_cast<unsigned>(std::stoul(std::string{raw.substr(5, 2)}));
    const auto day = static_cast<unsigned>(std::stoul(std::string{raw.substr(8, 2)}));
    const auto hour = static_cast<unsigned>(std::stoul(std::string{raw.substr(11, 2)}));
    const auto minute = static_cast<unsigned>(std::stoul(std::string{raw.substr(14, 2)}));
    const auto second = static_cast<unsigned>(std::stoul(std::string{raw.substr(17, 2)}));
    if (month < 1U || month > 12U || day < 1U || day > 31U || hour > 23U || minute > 59U || second > 60U) {
        throw std::runtime_error{"MRN timestamp has an invalid component: " + std::string{raw}};
    }

    std::size_t timezone_start = 19;
    while (timezone_start < raw.size() && raw[timezone_start] == '.') ++timezone_start;
    while (timezone_start < raw.size() && std::isdigit(static_cast<unsigned char>(raw[timezone_start])) != 0) {
        ++timezone_start;
    }
    std::int64_t offset_seconds = 0;
    if (timezone_start >= raw.size() || raw[timezone_start] == 'Z') {
        offset_seconds = 0;
    } else {
        const auto sign = raw[timezone_start] == '+' ? 1 : raw[timezone_start] == '-' ? -1 : 0;
        if (sign == 0 || raw.size() < timezone_start + 6 || raw[timezone_start + 3] != ':') {
            throw std::runtime_error{"MRN timestamp must include Z or an explicit UTC offset: " + std::string{raw}};
        }
        const auto offset_hour = std::stoi(std::string{raw.substr(timezone_start + 1, 2)});
        const auto offset_minute = std::stoi(std::string{raw.substr(timezone_start + 4, 2)});
        if (offset_hour > 23 || offset_minute > 59) throw std::runtime_error{"Invalid MRN timezone offset"};
        offset_seconds = static_cast<std::int64_t>(sign) *
            (static_cast<std::int64_t>(offset_hour) * 3600 + static_cast<std::int64_t>(offset_minute) * 60);
    }
    const auto epoch = days_from_civil(year, month, day) * 86400 + static_cast<std::int64_t>(hour) * 3600 +
        static_cast<std::int64_t>(minute) * 60 + static_cast<std::int64_t>(second) - offset_seconds;
    const auto date = date_string(year, month, day);
    const auto utc_days = epoch / 86400;
    const auto utc_seconds = epoch % 86400;
    int utc_year = 1970;
    unsigned utc_month = 1;
    unsigned utc_day = 1;
    bool found_utc_date = false;
    for (int candidate_year = 1970; candidate_year <= 2100; ++candidate_year) {
        for (unsigned candidate_month = 1; candidate_month <= 12; ++candidate_month) {
            const auto first = days_from_civil(candidate_year, candidate_month, 1);
            const auto next = candidate_month == 12
                ? days_from_civil(candidate_year + 1, 1, 1)
                : days_from_civil(candidate_year, candidate_month + 1, 1);
            if (utc_days >= first && utc_days < next) {
                utc_year = candidate_year;
                utc_month = candidate_month;
                utc_day = static_cast<unsigned>(utc_days - first + 1);
                found_utc_date = true;
                break;
            }
        }
        if (found_utc_date) break;
    }
    if (!found_utc_date) throw std::runtime_error{"MRN timestamp is outside supported range: " + std::string{raw}};
    const auto utc_hour = static_cast<unsigned>(utc_seconds / 3600);
    const auto utc_minute = static_cast<unsigned>((utc_seconds % 3600) / 60);
    const auto utc_second = static_cast<unsigned>(utc_seconds % 60);
    std::ostringstream normalized;
    normalized << date_string(utc_year, utc_month, utc_day) << 'T' << std::setfill('0') << std::setw(2) << utc_hour
               << ':' << std::setw(2) << utc_minute << ':' << std::setw(2) << utc_second << 'Z';
    const auto normalized_timestamp = normalized.str();
    return ParsedTimestamp{
        .epoch_seconds = epoch,
        .date = normalized_timestamp.substr(0, 10),
        .normalized = normalized_timestamp
    };
}

[[nodiscard]] int weekday(const std::int64_t days_since_epoch) {
    const auto value = (days_since_epoch + 4) % 7;
    return static_cast<int>(value < 0 ? value + 7 : value); // 0 = Sunday.
}

[[nodiscard]] unsigned first_sunday(const int year, const unsigned month) {
    const auto first_weekday = weekday(days_from_civil(year, month, 1));
    return static_cast<unsigned>(1 + ((7 - first_weekday) % 7));
}

[[nodiscard]] bool eastern_daylight_time(const int year, const unsigned month, const unsigned day) {
    const auto start_day = static_cast<unsigned>(first_sunday(year, 3) + 7U);
    const auto end_day = first_sunday(year, 11);
    if (month < 3U || month > 11U) return false;
    if (month > 3U && month < 11U) return true;
    return month == 3U ? day >= start_day : month == 11U ? day < end_day : false;
}

[[nodiscard]] std::int64_t close_cutoff_epoch(const std::string_view date) {
    const auto year = std::stoi(std::string{date.substr(0, 4)});
    const auto month = static_cast<unsigned>(std::stoul(std::string{date.substr(5, 2)}));
    const auto day = static_cast<unsigned>(std::stoul(std::string{date.substr(8, 2)}));
    const auto close_hour_utc = eastern_daylight_time(year, month, day) ? 20 : 21;
    return days_from_civil(year, month, day) * 86400 + static_cast<std::int64_t>(close_hour_utc) * 3600;
}

struct Calendar final {
    std::vector<std::string> dates;

    [[nodiscard]] std::string session_for(const ParsedTimestamp& timestamp) const {
        const auto position = timestamp.epoch_seconds <= close_cutoff_epoch(timestamp.date)
            ? std::ranges::lower_bound(dates, timestamp.date)
            : std::ranges::upper_bound(dates, timestamp.date);
        return position == dates.end() ? std::string{} : *position;
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
        const auto seconds = std::stoll(row[timestamp]);
        const auto days = seconds / 86400;
        // Market history timestamps are UTC epoch seconds. Converting the day number here
        // avoids using the machine's local timezone when assigning article sessions.
        const auto candidate = [&]() {
            for (int year = 1970; year <= 2100; ++year) {
                for (unsigned month = 1; month <= 12; ++month) {
                    const auto first = days_from_civil(year, month, 1);
                    const auto next = month == 12 ? days_from_civil(year + 1, 1, 1) : days_from_civil(year, month + 1, 1);
                    if (days >= first && days < next) {
                        return date_string(year, month, static_cast<unsigned>(days - first + 1));
                    }
                }
            }
            throw std::runtime_error{"Market calendar timestamp is outside supported range"};
        }();
        calendar.dates.push_back(candidate);
    }
    if (calendar.dates.empty()) throw std::runtime_error{"Market calendar is empty"};
    std::ranges::sort(calendar.dates);
    calendar.dates.erase(std::ranges::unique(calendar.dates).begin(), calendar.dates.end());
    return calendar;
}

struct MembershipIndex final {
    // Holdings history is stored as complete snapshots.  Resolving each
    // symbol independently to its latest row makes a departure from an ETF
    // look permanent, because the source uses a far-future effective_to
    // sentinel.  Select the complete snapshot governing the article date so
    // additions and removals are applied together.
    std::map<std::string, std::map<std::string, std::unordered_set<std::string>>> snapshots;

    [[nodiscard]] std::vector<std::string> sectors_for(
        const std::string_view symbol,
        const std::string_view date
    ) const {
        const auto snapshot = snapshots.lower_bound(std::string{date});
        if (snapshot == snapshots.begin()) return {};
        const auto& by_sector = std::prev(snapshot)->second;
        std::vector<std::string> result;
        result.reserve(by_sector.size());
        for (const auto& [sector, symbols] : by_sector) {
            if (symbols.contains(std::string{symbol})) result.push_back(sector);
        }
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
    static_cast<void>(effective_from);
    MembershipIndex result;
    while (true) {
        const auto row = reader.next();
        if (row.empty()) break;
        if (row.size() != header.size()) throw std::runtime_error{"Malformed sector holdings row"};
        if (row[sector].empty() || row[symbol].empty() || row[available_from].empty()) continue;
        if (row[effective_to] != "2099-12-31") {
            throw std::runtime_error{"Sector holdings effective_to must be the sentinel"};
        }
        result.snapshots[row[available_from]][row[sector]].insert(row[symbol]);
    }
    if (result.snapshots.empty()) throw std::runtime_error{"Sector holdings history is empty"};
    return result;
}

[[nodiscard]] std::unordered_map<std::string, std::string> load_entity_map(const std::filesystem::path& path) {
    CsvReader reader{path};
    const auto header = reader.next();
    const auto entity = column_index(header, "entity_id");
    const auto symbol = column_index(header, "symbol");
    std::unordered_map<std::string, std::string> result;
    while (true) {
        const auto row = reader.next();
        if (row.empty()) break;
        if (row.size() != header.size() || row[entity].empty() || row[symbol].empty()) {
            throw std::runtime_error{"Malformed MRN entity map row"};
        }
        result[row[entity]] = row[symbol];
    }
    if (result.empty()) throw std::runtime_error{"MRN entity map is empty"};
    return result;
}

[[nodiscard]] std::string json_string(const boost::json::object& object, const std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || found->value().is_null()) return {};
    if (found->value().is_string()) return std::string{found->value().as_string()};
    if (found->value().is_int64()) return std::to_string(found->value().as_int64());
    if (found->value().is_uint64()) return std::to_string(found->value().as_uint64());
    return boost::json::serialize(found->value());
}

[[nodiscard]] const boost::json::object& payload_object(const boost::json::value& value) {
    if (!value.is_object()) throw std::runtime_error{"MRN JSONL record is not an object"};
    const auto& object = value.as_object();
    for (const auto key : {std::string_view{"data"}, std::string_view{"payload"}, std::string_view{"message"}}) {
        const auto found = object.find(key);
        if (found != object.end() && found->value().is_object()) return found->value().as_object();
    }
    return object;
}

void append_subjects(
    const boost::json::value& value,
    std::vector<std::string>& result
) {
    if (value.is_array()) {
        for (const auto& item : value.as_array()) append_subjects(item, result);
        return;
    }
    if (value.is_string() || value.is_int64() || value.is_uint64()) {
        if (value.is_string()) result.emplace_back(value.as_string());
        else if (value.is_int64()) result.push_back(std::to_string(value.as_int64()));
        else result.push_back(std::to_string(value.as_uint64()));
        return;
    }
    if (!value.is_object()) return;
    const auto& object = value.as_object();
    for (const auto key : {std::string_view{"ric"}, std::string_view{"permId"}, std::string_view{"permid"},
                           std::string_view{"code"}, std::string_view{"id"}, std::string_view{"value"}}) {
        const auto found = object.find(key);
        if (found != object.end()) append_subjects(found->value(), result);
    }
}

[[nodiscard]] bool is_withdrawal(const std::string_view message_type, const std::string_view pub_status) {
    const auto message = lower_copy(std::string{message_type});
    const auto status = lower_copy(std::string{pub_status});
    for (const auto term : {std::string_view{"delete"}, std::string_view{"withdraw"}, std::string_view{"retract"},
                            std::string_view{"kill"}, std::string_view{"remove"}}) {
        if (message.find(term) != std::string::npos || status.find(term) != std::string::npos) return true;
    }
    return false;
}

struct Version final {
    std::string id;
    std::string alt_id;
    std::string first_created_raw;
    std::string version_created_raw;
    ParsedTimestamp version_created;
    std::uint64_t take_sequence{};
    std::string message_type;
    std::string pub_status;
    std::string headline;
    std::string body;
    std::string url;
    std::string provider;
    std::vector<std::string> subjects;
    bool withdrawn{};
};

struct Candidate final {
    const Version* version{};
    std::string trading_date;
};

[[nodiscard]] std::uint64_t json_uint(const boost::json::object& object, const std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || found->value().is_null()) return 0;
    if (found->value().is_uint64()) return found->value().as_uint64();
    if (found->value().is_int64()) return static_cast<std::uint64_t>(found->value().as_int64());
    if (found->value().is_string()) return std::stoull(std::string{found->value().as_string()});
    throw std::runtime_error{"MRN takeSequence is not numeric"};
}

[[nodiscard]] Version parse_version(const boost::json::value& value) {
    const auto& object = payload_object(value);
    Version result;
    result.id = json_string(object, "id");
    result.alt_id = json_string(object, "altId");
    result.first_created_raw = json_string(object, "firstCreated");
    result.version_created_raw = json_string(object, "versionCreated");
    result.message_type = json_string(object, "messageType");
    result.pub_status = json_string(object, "pubStatus");
    result.headline = json_string(object, "headline");
    result.body = json_string(object, "body");
    result.provider = json_string(object, "provider");
    result.url = json_string(object, "url");
    if (result.url.empty()) result.url = json_string(object, "canonicalUrl");
    if (result.id.empty() || result.version_created_raw.empty()) {
        throw std::runtime_error{"MRN record is missing required id or versionCreated"};
    }
    result.version_created = parse_timestamp(result.version_created_raw);
    result.take_sequence = json_uint(object, "takeSequence");
    const auto subjects = object.find("subjects");
    if (subjects != object.end()) append_subjects(subjects->value(), result.subjects);
    const auto entities = object.find("entities");
    if (entities != object.end()) append_subjects(entities->value(), result.subjects);
    result.withdrawn = is_withdrawal(result.message_type, result.pub_status);
    return result;
}

[[nodiscard]] std::vector<Version> load_versions(
    const std::filesystem::path& path,
    std::size_t& records_read,
    std::size_t& records_rejected
) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open MRN JSONL: " + path.string()};
    std::vector<Version> versions;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) continue;
        ++records_read;
        boost::system::error_code error;
        const auto value = boost::json::parse(line, error);
        if (error) throw std::runtime_error{"Invalid MRN JSON at line " + std::to_string(line_number) + ": " + error.message()};
        try {
            versions.push_back(parse_version(value));
        } catch (const std::exception&) {
            ++records_rejected;
        }
    }
    if (versions.empty()) throw std::runtime_error{"MRN JSONL produced no valid records"};
    std::ranges::sort(versions, [](const auto& left, const auto& right) {
        return std::tie(left.id, left.version_created.epoch_seconds, left.take_sequence) <
            std::tie(right.id, right.version_created.epoch_seconds, right.take_sequence);
    });
    return versions;
}

struct OutputRow final {
    std::string article_id;
    std::string original_id;
    std::string alt_id;
    std::string first_created;
    std::string version_created;
    std::uint64_t take_sequence{};
    std::string message_type;
    std::string pub_status;
    std::string published_at_utc;
    std::string trading_date;
    std::string sector;
    std::string symbol;
    std::string title;
    std::string body;
    std::string url;
    std::string provider;
    std::string content_hash;
};

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 8) {
            std::cout << "Usage: arrakis-build-mrn-sector-articles <mrn.jsonl> <entity_map.csv> "
                         "<sector_holdings_history.csv> <market_calendar.csv> <from-date> <to-date> <output.csv>\n";
            return 0;
        }
        const auto from_date = std::string{argv[5]};
        const auto to_date = std::string{argv[6]};
        if (from_date.empty() || to_date.empty() || from_date > to_date) throw std::invalid_argument{"Invalid date range"};
        const auto calendar = load_calendar(argv[4]);
        const auto entities = load_entity_map(argv[2]);
        const auto membership = load_membership(argv[3]);
        std::size_t records_read = 0;
        std::size_t records_rejected = 0;
        const auto versions = load_versions(argv[1], records_read, records_rejected);

        std::map<std::pair<std::string, std::string>, Candidate> selected;
        std::size_t version_groups = 0;
        std::size_t withdrawals = 0;
        std::size_t unknown_entities = 0;
        std::size_t membership_rejects = 0;
        std::size_t outside_range = 0;
        for (std::size_t index = 0; index < versions.size();) {
            const auto group_start = index;
            while (index < versions.size() && versions[index].id == versions[group_start].id) ++index;
            ++version_groups;
            std::vector<std::string> known_symbols;
            for (std::size_t version_index = group_start; version_index < index; ++version_index) {
                if (versions[version_index].withdrawn) continue;
                for (const auto& subject : versions[version_index].subjects) {
                    const auto symbol = entities.find(subject);
                    if (symbol != entities.end()) known_symbols.push_back(symbol->second);
                }
            }
            std::ranges::sort(known_symbols);
            known_symbols.erase(std::ranges::unique(known_symbols).begin(), known_symbols.end());
            for (std::size_t version_index = group_start; version_index < index; ++version_index) {
                const auto& version = versions[version_index];
                const auto trading_date = calendar.session_for(version.version_created);
                if (trading_date.empty() || trading_date < from_date || trading_date > to_date) {
                    ++outside_range;
                    continue;
                }
                if (version.withdrawn) {
                    ++withdrawals;
                    std::vector<std::string> withdrawal_symbols;
                    for (const auto& subject : version.subjects) {
                        const auto symbol = entities.find(subject);
                        if (symbol == entities.end()) continue;
                        withdrawal_symbols.push_back(symbol->second);
                    }
                    if (withdrawal_symbols.empty()) withdrawal_symbols = known_symbols;
                    std::ranges::sort(withdrawal_symbols);
                    withdrawal_symbols.erase(std::ranges::unique(withdrawal_symbols).begin(), withdrawal_symbols.end());
                    for (const auto& symbol : withdrawal_symbols) {
                        for (const auto& sector : membership.sectors_for(symbol, trading_date)) {
                            selected.erase({version.id + "|" + sector + "|" + symbol, trading_date});
                        }
                    }
                    continue;
                }
                std::vector<std::pair<std::string, std::string>> resolved;
                for (const auto& subject : version.subjects) {
                    const auto symbol = entities.find(subject);
                    if (symbol == entities.end()) {
                        ++unknown_entities;
                        continue;
                    }
                    const auto sectors = membership.sectors_for(symbol->second, trading_date);
                    if (sectors.empty()) {
                        ++membership_rejects;
                        continue;
                    }
                    for (const auto& sector : sectors) resolved.emplace_back(sector, symbol->second);
                }
                std::ranges::sort(resolved);
                resolved.erase(std::ranges::unique(resolved).begin(), resolved.end());
                for (const auto& [sector, symbol] : resolved) {
                    const auto key = std::make_pair(version.id + "|" + sector + "|" + symbol, trading_date);
                    selected[key] = Candidate{.version = &version, .trading_date = trading_date};
                }
            }
        }

        std::vector<OutputRow> output_rows;
        for (const auto& [key, candidate] : selected) {
            const auto& version = *candidate.version;
            const auto delimiter = key.first.rfind('|');
            const auto sector_delimiter = key.first.find('|');
            const auto sector = key.first.substr(sector_delimiter + 1, delimiter - sector_delimiter - 1);
            const auto symbol = key.first.substr(delimiter + 1);
            const auto first_created = version.first_created_raw.empty()
                ? version.version_created.normalized
                : parse_timestamp(version.first_created_raw).normalized;
            const auto content_hash = sha256(version.id + "|" + version.version_created.normalized + "|" +
                                             std::to_string(version.take_sequence) + "|" + version.headline + "|" + version.body);
            output_rows.push_back(OutputRow{
                .article_id = "mrn:" + version.id + ":" + std::to_string(version.take_sequence) + ":" + symbol,
                .original_id = version.id,
                .alt_id = version.alt_id,
                .first_created = first_created,
                .version_created = version.version_created.normalized,
                .take_sequence = version.take_sequence,
                .message_type = version.message_type,
                .pub_status = version.pub_status,
                .published_at_utc = version.version_created.normalized,
                .trading_date = candidate.trading_date,
                .sector = sector,
                .symbol = symbol,
                .title = version.headline,
                .body = version.body,
                .url = version.url,
                .provider = version.provider,
                .content_hash = content_hash,
            });
        }
        std::ranges::sort(output_rows, [](const auto& left, const auto& right) {
            return std::tie(left.trading_date, left.sector, left.symbol, left.version_created) <
                std::tie(right.trading_date, right.sector, right.symbol, right.version_created);
        });
        const auto output_path = std::filesystem::path{argv[7]};
        if (!output_path.parent_path().empty()) std::filesystem::create_directories(output_path.parent_path());
        std::ofstream output{output_path};
        if (!output) throw std::runtime_error{"Could not write MRN article output: " + output_path.string()};
        output << "article_id,original_id,alt_id,first_created,version_created,take_sequence,message_type,pub_status,"
                  "published_at_utc,timestamp_quality,trading_date,sector,symbol,title,summary,url,publisher,content_hash\n";
        for (const auto& row : output_rows) {
            output << csv_escape(row.article_id) << ',' << csv_escape(row.original_id) << ',' << csv_escape(row.alt_id)
                   << ',' << row.first_created << ',' << row.version_created << ',' << row.take_sequence << ','
                   << csv_escape(row.message_type) << ',' << csv_escape(row.pub_status) << ',' << row.published_at_utc
                   << ",mrn_version_created_utc," << row.trading_date << ',' << row.sector << ',' << row.symbol << ','
                   << csv_escape(row.title) << ',' << csv_escape(row.body) << ',' << csv_escape(row.url) << ','
                   << csv_escape(row.provider) << ',' << row.content_hash << '\n';
        }
        std::ofstream manifest{std::string{output_path} + ".manifest.json"};
        if (!manifest) throw std::runtime_error{"Could not write MRN manifest"};
        manifest << "{\n"
                 << "  \"source\": \"LSEG Machine Readable News JSONL contract\",\n"
                 << "  \"records_read\": " << records_read << ",\n"
                 << "  \"records_rejected\": " << records_rejected << ",\n"
                 << "  \"version_groups\": " << version_groups << ",\n"
                 << "  \"rows_written\": " << output_rows.size() << ",\n"
                 << "  \"withdrawals_applied\": " << withdrawals << ",\n"
                 << "  \"unknown_entity_references\": " << unknown_entities << ",\n"
                 << "  \"membership_rejects\": " << membership_rejects << ",\n"
                 << "  \"outside_range_versions\": " << outside_range << ",\n"
                 << "  \"date_from\": \"" << from_date << "\",\n"
                 << "  \"date_to\": \"" << to_date << "\",\n"
                 << "  \"cutoff_policy\": \"latest versionCreated at or before 16:00 America/New_York, mapped to the first eligible market session; after-close versions map to the next session\",\n"
                 << "  \"membership_policy\": \"available_from must be strictly before the assigned trading date; effective interval must contain the trading date\",\n"
                 << "  \"version_policy\": \"versions are ordered by versionCreated then takeSequence; later corrections affect only their assigned future session; withdrawal messages erase the current session candidate\",\n"
                 << "  \"timestamp_quality\": \"mrn_version_created_utc\"\n}\n";
        std::cout << "MRN sector articles: " << output_rows.size() << " rows from " << records_read << " records\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-build-mrn-sector-articles: " << error.what() << '\n';
        return 1;
    }
}
