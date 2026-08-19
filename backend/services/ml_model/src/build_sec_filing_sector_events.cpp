#include <boost/json.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

[[nodiscard]] std::vector<std::string> read_record(std::istream& input) {
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

[[nodiscard]] std::string sha256(std::string_view value) {
    unsigned char digest[SHA256_DIGEST_LENGTH]{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
}

[[nodiscard]] std::string padded_cik(const std::string_view value) {
    const auto number = std::stoull(std::string{value});
    std::ostringstream output;
    output << std::setw(10) << std::setfill('0') << number;
    return output.str();
}

[[nodiscard]] std::string date_from_epoch(const std::string_view value) {
    const auto seconds = static_cast<std::time_t>(std::stoll(std::string{value}));
    std::tm utc{};
    if (gmtime_r(&seconds, &utc) == nullptr) throw std::runtime_error{"Could not convert calendar epoch"};
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%d");
    return output.str();
}

[[nodiscard]] std::int64_t days_from_civil(const int year, const unsigned month, const unsigned day) {
    const auto adjusted_year = static_cast<std::int64_t>(year) - (month <= 2U ? 1 : 0);
    const auto era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const auto year_of_era = adjusted_year - era * 400;
    const auto month_number = static_cast<std::int64_t>(month);
    const auto day_of_year = (153 * (month_number + (month_number > 2 ? -3 : 9)) + 2) / 5 +
        static_cast<std::int64_t>(day) - 1;
    const auto day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
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
    return month == 3U ? day >= start_day : day < end_day;
}

[[nodiscard]] std::int64_t utc_timestamp_seconds(const std::string_view value) {
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':') {
        throw std::invalid_argument{"Invalid SEC acceptance timestamp"};
    }
    const auto year = std::stoi(std::string{value.substr(0, 4)});
    const auto month = static_cast<unsigned>(std::stoul(std::string{value.substr(5, 2)}));
    const auto day = static_cast<unsigned>(std::stoul(std::string{value.substr(8, 2)}));
    const auto hour = static_cast<unsigned>(std::stoul(std::string{value.substr(11, 2)}));
    const auto minute = static_cast<unsigned>(std::stoul(std::string{value.substr(14, 2)}));
    const auto second = static_cast<unsigned>(std::stoul(std::string{value.substr(17, 2)}));
    if (hour > 23U || minute > 59U || second > 60U) throw std::invalid_argument{"Invalid SEC timestamp component"};
    return days_from_civil(year, month, day) * 86400 + static_cast<std::int64_t>(hour) * 3600 +
           static_cast<std::int64_t>(minute) * 60 + static_cast<std::int64_t>(second);
}

[[nodiscard]] std::int64_t eastern_cutoff_seconds(const std::string_view date, const int hour, const int minute) {
    const auto year = std::stoi(std::string{date.substr(0, 4)});
    const auto month = static_cast<unsigned>(std::stoul(std::string{date.substr(5, 2)}));
    const auto day = static_cast<unsigned>(std::stoul(std::string{date.substr(8, 2)}));
    const auto utc_offset = eastern_daylight_time(year, month, day) ? 4 : 5;
    return days_from_civil(year, month, day) * 86400 +
           static_cast<std::int64_t>(hour + utc_offset) * 3600 + static_cast<std::int64_t>(minute) * 60;
}

struct SelectedHolding final {
    std::string sector;
    std::string symbol;
    std::string cik;
};

struct Membership final {
    std::string sector;
    std::string symbol;
    std::string effective_from;
    std::string available_from;
    std::string effective_to;
};

struct Calendar final {
    std::vector<std::string> dates;

    struct PreopenAssignment final {
        std::string event_session;
        std::string feature_date;
    };

    [[nodiscard]] std::string first_session_after(const std::string_view date) const {
        const auto found = std::ranges::upper_bound(dates, std::string{date});
        if (found == dates.end()) return {};
        return *found;
    }

    [[nodiscard]] std::optional<PreopenAssignment> preopen_assignment(
        const std::int64_t acceptance_seconds
    ) const {
        for (std::size_t index = 1; index < dates.size(); ++index) {
            const auto& previous_session = dates[index - 1];
            const auto& event_session = dates[index];
            const auto after_previous_close = eastern_cutoff_seconds(previous_session, 16, 0);
            const auto preopen_cutoff = eastern_cutoff_seconds(event_session, 9, 20);
            if (acceptance_seconds > after_previous_close && acceptance_seconds <= preopen_cutoff) {
                return PreopenAssignment{.event_session = event_session, .feature_date = previous_session};
            }
        }
        return std::nullopt;
    }
};

struct FilingEvent final {
    std::string article_id;
    std::string published_at_raw;
    std::string published_at_utc;
    std::string source_calendar_date;
    std::string trading_date;
    std::string sector;
    std::string symbol;
    std::string title;
    std::string url;
    std::string content_hash;
    std::string form;
    std::string accession;
};

[[nodiscard]] std::vector<SelectedHolding> load_selected_holdings(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open selected holdings: " + path.string()};
    std::vector<SelectedHolding> result;
    while (true) {
        const auto row = read_record(input);
        if (row.empty()) break;
        if (row.size() < 4 || row[0] == "sector") continue;
        if (row[3].empty()) continue;
        result.push_back(SelectedHolding{.sector = row[0], .symbol = row[1], .cik = padded_cik(row[3])});
    }
    if (result.empty()) throw std::runtime_error{"Selected holdings contain no CIKs"};
    return result;
}

[[nodiscard]] std::vector<Membership> load_membership(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open sector holdings history: " + path.string()};
    const auto header = read_record(input);
    const auto sector = column_index(header, "sector");
    const auto symbol = column_index(header, "symbol");
    const auto effective_from = column_index(header, "effective_from");
    const auto available_from = column_index(header, "available_from");
    const auto effective_to = column_index(header, "effective_to");
    std::vector<Membership> result;
    while (true) {
        const auto row = read_record(input);
        if (row.empty()) break;
        if (row.size() != header.size()) throw std::runtime_error{"Malformed sector holdings row"};
        result.push_back(Membership{
            .sector = row[sector], .symbol = row[symbol], .effective_from = row[effective_from],
            .available_from = row[available_from], .effective_to = row[effective_to]
        });
    }
    return result;
}

[[nodiscard]] Calendar load_calendar(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open market calendar: " + path.string()};
    const auto header = read_record(input);
    const auto timestamp = column_index(header, "timestamp_utc");
    Calendar result;
    while (true) {
        const auto row = read_record(input);
        if (row.empty()) break;
        if (row.size() <= timestamp) throw std::runtime_error{"Malformed market calendar row"};
        result.dates.push_back(date_from_epoch(row[timestamp]));
    }
    if (result.dates.empty()) throw std::runtime_error{"Market calendar is empty"};
    return result;
}

[[nodiscard]] bool is_member(
    const std::vector<Membership>& history,
    const std::string_view sector,
    const std::string_view symbol,
    const std::string_view date
) {
    const Membership* best = nullptr;
    for (const auto& row : history) {
        if (row.sector != sector || row.symbol != symbol || row.available_from >= date ||
            row.effective_from > date || row.effective_to < date) continue;
        if (best == nullptr || row.available_from > best->available_from) best = &row;
    }
    return best != nullptr;
}

[[nodiscard]] bool supported_form(const std::string_view form) {
    return form == "8-K" || form == "8-K/A" || form == "10-K" || form == "10-K/A" ||
           form == "10-Q" || form == "10-Q/A" || form == "6-K" || form == "20-F" || form == "40-F";
}

[[nodiscard]] std::string json_string(const boost::json::array& values, const std::size_t index) {
    if (index >= values.size() || !values[index].is_string()) return {};
    return std::string{values[index].as_string()};
}

[[nodiscard]] std::string archive_url(
    const std::string_view cik,
    const std::string_view accession,
    const std::string_view document
) {
    auto accession_path = std::string{accession};
    accession_path.erase(std::remove(accession_path.begin(), accession_path.end(), '-'), accession_path.end());
    const auto numeric_cik = std::to_string(std::stoull(std::string{cik}));
    return "https://www.sec.gov/Archives/edgar/data/" + numeric_cik + "/" + accession_path + "/" + std::string{document};
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 8 && argc != 9) {
            std::cout << "Usage: arrakis-build-sec-filing-sector-events <selected_holdings.csv> "
                         "<sector_holdings_history.csv> <submissions_dir> <market_calendar.csv> "
                         "<from-date> <to-date> <output.csv> [preopen-next-close]\n";
            return 0;
        }
        const auto preopen_policy = argc == 9 && std::string_view{argv[8]} == "preopen-next-close";
        if (argc == 9 && !preopen_policy) throw std::invalid_argument{"Unknown SEC event policy"};
        const auto selected = load_selected_holdings(argv[1]);
        const auto membership = load_membership(argv[2]);
        const auto calendar = load_calendar(argv[4]);
        std::vector<FilingEvent> events;
        std::size_t filings_read = 0;
        std::size_t skipped_forms = 0;
        std::size_t skipped_dates = 0;
        std::size_t skipped_membership = 0;
        std::size_t missing_submissions = 0;

        for (const auto& holding : selected) {
            const auto path = std::filesystem::path{argv[3]} / ("CIK" + holding.cik + ".json");
            if (!std::filesystem::exists(path)) {
                ++missing_submissions;
                continue;
            }
            std::ifstream input{path};
            std::ostringstream content;
            content << input.rdbuf();
            boost::system::error_code parse_error;
            const auto root = boost::json::parse(content.str(), parse_error);
            if (parse_error || !root.is_object()) throw std::runtime_error{"Invalid SEC submissions JSON: " + path.string()};
            const auto& root_object = root.as_object();
            const auto& filings = root_object.at("filings").as_object();
            const auto& recent = filings.at("recent").as_object();
            const auto& forms = recent.at("form").as_array();
            const auto& filing_dates = recent.at("filingDate").as_array();
            const auto& acceptance_times = recent.at("acceptanceDateTime").as_array();
            const auto& accessions = recent.at("accessionNumber").as_array();
            const auto& documents = recent.at("primaryDocument").as_array();
            const auto count = std::min({forms.size(), filing_dates.size(), acceptance_times.size(), accessions.size(), documents.size()});
            for (std::size_t index = 0; index < count; ++index) {
                ++filings_read;
                const auto form = json_string(forms, index);
                const auto acceptance = json_string(acceptance_times, index);
                const auto accession = json_string(accessions, index);
                const auto document = json_string(documents, index);
                if (!supported_form(form)) {
                    ++skipped_forms;
                    continue;
                }
                if (acceptance.size() < 20 || acceptance[4] != '-' || acceptance[7] != '-') {
                    ++skipped_dates;
                    continue;
                }
                const auto source_date = acceptance.substr(0, 10);
                std::string trading_date;
                if (preopen_policy) {
                    const auto assignment = calendar.preopen_assignment(utc_timestamp_seconds(acceptance));
                    if (!assignment.has_value() || assignment->event_session < argv[5] || assignment->event_session > argv[6]) {
                        ++skipped_dates;
                        continue;
                    }
                    if (!is_member(membership, holding.sector, holding.symbol, assignment->event_session)) {
                        ++skipped_membership;
                        continue;
                    }
                    // The pooled panel labels row t with close[t+1] > close[t].
                    // A filing available before the next open therefore joins row t.
                    trading_date = assignment->feature_date;
                } else {
                    if (source_date < argv[5] || source_date > argv[6]) {
                        ++skipped_dates;
                        continue;
                    }
                    if (!is_member(membership, holding.sector, holding.symbol, source_date)) {
                        ++skipped_membership;
                        continue;
                    }
                    trading_date = calendar.first_session_after(source_date);
                    if (trading_date.empty()) continue;
                }
                const auto key = holding.sector + "|" + holding.symbol + "|" + accession;
                const auto hash = sha256(key);
                events.push_back(FilingEvent{
                    .article_id = "sec:sha256:" + hash,
                    .published_at_raw = acceptance,
                    .published_at_utc = acceptance.substr(0, 19) + "Z",
                    .source_calendar_date = source_date,
                    .trading_date = trading_date,
                    .sector = holding.sector,
                    .symbol = holding.symbol,
                    .title = "SEC " + form + " " + document,
                    .url = archive_url(holding.cik, accession, document),
                    .content_hash = hash,
                    .form = form,
                    .accession = accession,
                });
            }
        }

        std::ranges::sort(events, [](const auto& left, const auto& right) {
            return std::tie(left.trading_date, left.sector, left.published_at_utc, left.accession) <
                   std::tie(right.trading_date, right.sector, right.published_at_utc, right.accession);
        });
        std::ofstream output{argv[7]};
        if (!output) throw std::runtime_error{"Could not write SEC filing events: " + std::string{argv[7]}};
        output << "article_id,published_at_raw,published_at_utc,timestamp_quality,source_calendar_date,trading_date,sector,symbol,title,summary,url,publisher,content_hash,form,accession\n";
        for (const auto& event : events) {
            output << event.article_id << ',' << csv_escape(event.published_at_raw) << ',' << event.published_at_utc
                   << ",sec_acceptance_utc," << event.source_calendar_date << ',' << event.trading_date << ','
                   << event.sector << ',' << event.symbol << ',' << csv_escape(event.title) << ",," << csv_escape(event.url)
                   << ",SEC," << event.content_hash << ',' << event.form << ',' << event.accession << '\n';
        }
        std::ofstream manifest{std::string{argv[7]} + ".manifest.json"};
        if (!manifest) throw std::runtime_error{"Could not write SEC filing event manifest"};
        manifest << "{\n"
                 << "  \"source\": \"SEC EDGAR submissions\",\n"
                 << "  \"rows_read\": " << filings_read << ",\n"
                 << "  \"rows_written\": " << events.size() << ",\n"
                 << "  \"skipped_forms\": " << skipped_forms << ",\n"
                 << "  \"skipped_dates\": " << skipped_dates << ",\n"
                 << "  \"skipped_membership\": " << skipped_membership << ",\n"
                 << "  \"missing_submissions\": " << missing_submissions << ",\n"
                 << "  \"timestamp_quality\": \"sec_acceptance_utc\",\n"
                 << "  \"assignment_policy\": \""
                 << (preopen_policy
                         ? "accepted in (prior session 16:00 ET, event session 09:20 ET] and joined to prior-session feature row"
                         : "first SPY session strictly after SEC acceptance calendar date")
                 << "\",\n"
                 << "  \"date_from\": \"" << argv[5] << "\",\n"
                 << "  \"date_to\": \"" << argv[6] << "\"\n}\n";
        std::cout << "SEC filing events: " << events.size() << " rows from " << selected.size() << " selected holdings\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-build-sec-filing-sector-events: " << error.what() << '\n';
        return 1;
    }
}
