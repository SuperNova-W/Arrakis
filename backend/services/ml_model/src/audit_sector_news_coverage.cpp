#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
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
    if (found == header.end()) throw std::runtime_error{"Missing sector news column: " + std::string{name}};
    return static_cast<std::size_t>(std::distance(header.begin(), found));
}

[[nodiscard]] std::string json_escape(const std::string& value) {
    std::string escaped;
    for (const auto character : value) {
        if (character == '\\' || character == '"') escaped.push_back('\\');
        escaped.push_back(character);
    }
    return escaped;
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 3) {
            std::cout << "Usage: arrakis-audit-sector-news-coverage <sector_articles.csv> <output.json>\n";
            return 0;
        }
        std::ifstream input{argv[1]};
        if (!input) throw std::runtime_error{"Could not open sector articles: " + std::string{argv[1]}};
        const auto header = read_record(input);
        const auto date_index = column_index(header, "trading_date");
        const auto sector_index = column_index(header, "sector");
        const auto article_index = column_index(header, "article_id");
        const auto content_index = column_index(header, "content_hash");

        std::size_t rows = 0;
        std::map<std::string, std::set<std::string>> sectors_by_date;
        std::map<std::string, std::set<std::string>> sessions_by_content;
        std::set<std::string> article_ids;
        std::map<std::string, std::size_t> rows_by_sector;
        for (auto fields = read_record(input); !fields.empty(); fields = read_record(input)) {
            const auto max_index = std::max({date_index, sector_index, article_index, content_index});
            if (fields.size() <= max_index) throw std::runtime_error{"Malformed sector article row"};
            ++rows;
            sectors_by_date[fields[date_index]].insert(fields[sector_index]);
            ++rows_by_sector[fields[sector_index]];
            article_ids.insert(fields[article_index]);
            sessions_by_content[fields[content_index]].insert(fields[date_index]);
        }

        std::size_t dates_at_least_3 = 0;
        std::size_t dates_at_least_8 = 0;
        std::size_t max_active = 0;
        for (const auto& [date, sectors] : sectors_by_date) {
            static_cast<void>(date);
            max_active = std::max(max_active, sectors.size());
            if (sectors.size() >= 3) ++dates_at_least_3;
            if (sectors.size() >= 8) ++dates_at_least_8;
        }
        std::size_t content_session_conflicts = 0;
        for (const auto& [content_hash, sessions] : sessions_by_content) {
            static_cast<void>(content_hash);
            if (sessions.size() > 1) ++content_session_conflicts;
        }

        const std::string conflict_path = std::string{argv[2]} + ".conflicts.csv";
        std::ofstream conflicts{conflict_path};
        if (!conflicts) throw std::runtime_error{"Could not write conflict report: " + conflict_path};
        conflicts << "content_hash,sessions\n";
        for (const auto& [content_hash, sessions] : sessions_by_content) {
            if (sessions.size() <= 1) continue;
            conflicts << content_hash << ',';
            std::size_t session_index = 0;
            for (const auto& session : sessions) {
                conflicts << session << (++session_index == sessions.size() ? "\n" : ";");
            }
        }

        std::ofstream output{argv[2]};
        if (!output) throw std::runtime_error{"Could not write coverage audit: " + std::string{argv[2]}};
        output << "{\n"
               << "  \"input\": \"" << json_escape(argv[1]) << "\",\n"
               << "  \"rows\": " << rows << ",\n"
               << "  \"unique_article_ids\": " << article_ids.size() << ",\n"
               << "  \"eligible_dates\": " << sectors_by_date.size() << ",\n"
               << "  \"dates_with_at_least_3_active_sectors\": " << dates_at_least_3 << ",\n"
               << "  \"dates_with_at_least_8_active_sectors\": " << dates_at_least_8 << ",\n"
               << "  \"max_active_sectors_on_date\": " << max_active << ",\n"
               << "  \"content_hashes_spanning_multiple_sessions\": " << content_session_conflicts << ",\n"
               << "  \"conflict_report\": \"" << json_escape(conflict_path) << "\",\n"
               << "  \"first_session\": \"" << (sectors_by_date.empty() ? "" : sectors_by_date.begin()->first)
               << "\",\n"
               << "  \"last_session\": \"" << (sectors_by_date.empty() ? "" : sectors_by_date.rbegin()->first)
               << "\",\n"
               << "  \"rows_by_sector\": {\n";
        std::size_t index = 0;
        for (const auto& [sector, count] : rows_by_sector) {
            output << "    \"" << sector << "\": " << count
                   << (++index == rows_by_sector.size() ? "\n" : ",\n");
        }
        output << "  }\n}\n";
        std::cout << "Sector coverage: " << rows << " rows, " << sectors_by_date.size()
                  << " dates, " << dates_at_least_3 << " dates with >=3 active sectors, max "
                  << max_active << ", content-session conflicts " << content_session_conflicts << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-audit-sector-news-coverage: " << error.what() << '\n';
        return 1;
    }
}
