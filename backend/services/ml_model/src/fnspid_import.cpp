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
#include <unordered_set>
#include <vector>

namespace {

struct Holding final {
    std::string from;
    std::string to;
};

struct Options final {
    std::filesystem::path input;
    std::filesystem::path holdings;
    std::filesystem::path output{"data/fnspid/normalized/xlk_articles.csv"};
    std::filesystem::path manifest{"data/fnspid/manifests/import.json"};
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

[[nodiscard]] std::string previous_date(const std::string& value) {
    std::tm parsed{};
    std::istringstream input{value};
    input >> std::get_time(&parsed, "%Y-%m-%d");
    if (input.fail()) throw std::runtime_error{"Invalid holdings date: " + value};
    const auto timestamp = timegm(&parsed) - 24 * 60 * 60;
    std::tm output{};
    gmtime_r(&timestamp, &output);
    char formatted[11]{};
    std::strftime(formatted, sizeof(formatted), "%Y-%m-%d", &output);
    return formatted;
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
        else if (argument == "--from") options.from = argv[++index];
        else if (argument == "--to") options.to = argv[++index];
        else if (argument == "--help") {
            std::cout << "Usage: arrakis-import-fnspid --input <csv> --holdings <csv> [options]\n"
                      << "  --output <csv>       Normalized output\n"
                      << "  --manifest <json>    Import accounting manifest\n"
                      << "  --from <YYYY-MM-DD>  Inclusive date (default 2016-01-01)\n"
                      << "  --to <YYYY-MM-DD>    Inclusive date (default 2023-12-31)\n";
            std::exit(0);
        } else {
            throw std::invalid_argument{"Unknown option: " + std::string{argument}};
        }
    }
    if (options.input.empty() || options.holdings.empty()) {
        throw std::invalid_argument{
            "--input and --holdings are required; current holdings are never used as a fallback"};
    }
    return options;
}

[[nodiscard]] std::map<std::string, std::vector<Holding>> load_holdings(
    const std::filesystem::path& path
) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open holdings history: " + path.string()};
    const auto header = read_record(input);
    if (header.size() < 3 || header[0] != "symbol" || header[1] != "effective_from" ||
        header[2] != "effective_to") {
        throw std::runtime_error{
            "Holdings history must start with symbol,effective_from,effective_to"};
    }
    std::map<std::string, std::vector<Holding>> result;
    for (auto row = read_record(input); !row.empty(); row = read_record(input)) {
        if (row.size() < 3 || row[0].empty() || row[1].size() < 10 || row[2].size() < 10) {
            throw std::runtime_error{"Malformed holdings-history row"};
        }
        result[row[0]].push_back(Holding{row[1].substr(0, 10), row[2].substr(0, 10)});
    }
    if (result.empty()) throw std::runtime_error{"Holdings history is empty"};
    for (auto& [symbol, rows] : result) {
        static_cast<void>(symbol);
        std::ranges::sort(rows, [](const auto& left, const auto& right) {
            return left.from < right.from;
        });
        for (std::size_t index = 0; index + 1 < rows.size(); ++index) {
            if (rows[index].to == "2099-12-31") rows[index].to = previous_date(rows[index + 1].from);
            if (rows[index].to >= rows[index + 1].from) {
                throw std::runtime_error{"Overlapping holdings intervals for " + symbol};
            }
        }
    }
    return result;
}

[[nodiscard]] bool held_on(
    const std::map<std::string, std::vector<Holding>>& holdings,
    const std::string& symbol,
    const std::string& date
) {
    const auto found = holdings.find(symbol);
    if (found == holdings.end()) return false;
    return std::ranges::any_of(found->second, [&](const Holding& holding) {
        return holding.from <= date && date <= holding.to;
    });
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto holdings = load_holdings(options.holdings);
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

        std::size_t rows = 0;
        std::size_t written = 0;
        std::size_t skipped = 0;
        std::size_t duplicates = 0;
        std::size_t missing_timestamp = 0;
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
            if (date < options.from || date > options.to) continue;
            const auto symbol = trim(row[symbol_index]);
            const auto title = trim(row[title_index]);
            if (symbol.empty() || title.empty() || !held_on(holdings, symbol, date)) {
                ++skipped;
                continue;
            }
            const auto summary = trim(row[summary_index]);
            const auto content = lower(title + " " + summary);
            const auto content_hash = sha256(content);
            const auto identity = row[url_index].empty() ? content_hash : trim(row[url_index]);
            if (!seen.insert(identity).second) {
                ++duplicates;
                continue;
            }
            const auto article_id = sha256(symbol + "|" + date + "|" + identity);
            output << article_id << ',' << normalize_utc_timestamp(raw_date) << ',' << date << ',' << symbol << ','
                   << csv_escape(title) << ',' << csv_escape(summary) << ','
                   << csv_escape(trim(row[url_index])) << ',' << csv_escape(trim(row[publisher_index]))
                   << ',' << content_hash << '\n';
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
                 << "  \"holdings_fallback_used\": false,\n"
                 << "  \"holdings_interval_policy\": \"snapshot applies from effective_from until the day before the next snapshot\"\n}\n";
        std::cout << "FNSPID import complete: " << written << " rows written, " << skipped
                  << " skipped, " << duplicates << " duplicates\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FNSPID import failed: " << error.what() << '\n';
        return 1;
    }
}
