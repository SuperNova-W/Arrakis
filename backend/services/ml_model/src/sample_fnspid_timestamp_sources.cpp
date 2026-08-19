#include <algorithm>
#include <fstream>
#include <iostream>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
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
    if (quoted) throw std::runtime_error{"Sector news ended inside a quoted field"};
    if (!field.empty() || !fields.empty()) fields.push_back(field);
    return fields;
}

[[nodiscard]] std::size_t column_index(
    const std::vector<std::string>& header,
    const std::string_view name
) {
    const auto found = std::ranges::find(header, name);
    if (found == header.end()) throw std::runtime_error{"Missing column: " + std::string{name}};
    return static_cast<std::size_t>(std::distance(header.begin(), found));
}

[[nodiscard]] std::string year_month_hour(const std::string& timestamp) {
    if (timestamp.size() < 13) return "invalid";
    return timestamp.substr(0, 7) + "|" + timestamp.substr(11, 2);
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

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 4) {
            std::cout << "Usage: arrakis-sample-fnspid-timestamp-sources <sector_articles.csv> <output.csv> <per-source-cap>\n";
            return 0;
        }
        const auto cap = static_cast<std::size_t>(std::stoull(argv[3]));
        std::ifstream input{argv[1]};
        if (!input) throw std::runtime_error{"Could not open sector articles"};
        const auto header = read_record(input);
        const auto article_index = column_index(header, "article_id");
        const auto raw_index = column_index(header, "published_at_raw");
        const auto source_date_index = column_index(header, "source_calendar_date");
        const auto sector_index = column_index(header, "sector");
        const auto symbol_index = column_index(header, "symbol");
        const auto title_index = column_index(header, "title");
        const auto url_index = column_index(header, "url");
        const auto publisher_index = column_index(header, "publisher");
        std::ofstream output{argv[2]};
        if (!output) throw std::runtime_error{"Could not write timestamp sample"};
        output << "article_id,published_at_raw,source_calendar_date,sector,symbol,title,url,publisher\n";
        std::unordered_map<std::string, std::size_t> counts;
        std::size_t written = 0;
        for (auto row = read_record(input); !row.empty(); row = read_record(input)) {
            const auto max_index = std::max({article_index, raw_index, source_date_index, sector_index,
                                             symbol_index, title_index, url_index, publisher_index});
            if (row.size() <= max_index) throw std::runtime_error{"Malformed sector article row"};
            const auto key = row[publisher_index] + "|" + year_month_hour(row[raw_index]);
            auto& count = counts[key];
            if (count >= cap) continue;
            ++count;
            output << row[article_index] << ',' << csv_escape(row[raw_index]) << ','
                   << row[source_date_index] << ',' << row[sector_index] << ',' << row[symbol_index]
                   << ',' << csv_escape(row[title_index]) << ',' << csv_escape(row[url_index]) << ','
                   << csv_escape(row[publisher_index]) << '\n';
            ++written;
        }
        std::cout << "Wrote " << written << " timestamp-source samples\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-sample-fnspid-timestamp-sources: " << error.what() << '\n';
        return 1;
    }
}
