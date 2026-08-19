#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
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
    if (quoted) throw std::runtime_error{"SEC events CSV ended inside a quoted field"};
    if (!field.empty() || !fields.empty()) fields.push_back(field);
    return fields;
}

[[nodiscard]] std::size_t column_index(
    const std::vector<std::string>& header,
    const std::string_view name
) {
    const auto found = std::ranges::find(header, name);
    if (found == header.end()) throw std::runtime_error{"Missing SEC event column: " + std::string{name}};
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

[[nodiscard]] std::string lower_copy(std::string value) {
    std::ranges::transform(value, value.begin(), [](const char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    });
    return value;
}

[[nodiscard]] bool looks_like_exhibit_99(const std::string_view href) {
    const auto lower = lower_copy(std::string{href});
    if (lower.find("ex-99") != std::string::npos || lower.find("exhibit99") != std::string::npos ||
        lower.find("exhibit-99") != std::string::npos) {
        return true;
    }
    for (std::size_t position = 0; position + 3 < lower.size(); ++position) {
        if (lower[position] != '9' || lower[position + 1] != '9') continue;
        const auto separator = lower[position + 2];
        const auto suffix = lower[position + 3];
        if ((separator == '.' || separator == '_' || separator == '-') &&
            suffix >= '0' && suffix <= '9') {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::vector<std::string> href_values(const std::string& html) {
    std::vector<std::string> values;
    const auto lower = lower_copy(html);
    std::size_t search_from = 0;
    while (true) {
        const auto href_position = lower.find("href", search_from);
        if (href_position == std::string::npos) break;
        auto position = href_position + 4;
        while (position < html.size() && std::isspace(static_cast<unsigned char>(html[position])) != 0) ++position;
        if (position >= html.size() || html[position] != '=') {
            search_from = href_position + 4;
            continue;
        }
        ++position;
        while (position < html.size() && std::isspace(static_cast<unsigned char>(html[position])) != 0) ++position;
        if (position >= html.size()) break;
        const auto quote = html[position];
        if (quote != '\'' && quote != '"') {
            search_from = position + 1;
            continue;
        }
        const auto end = html.find(quote, position + 1);
        if (end == std::string::npos) break;
        values.emplace_back(html.substr(position + 1, end - position - 1));
        search_from = end + 1;
    }
    return values;
}

[[nodiscard]] std::string url_directory(const std::string_view url) {
    const auto slash = url.rfind('/');
    if (slash == std::string::npos) return {};
    return std::string{url.substr(0, slash + 1)};
}

[[nodiscard]] std::string resolve_url(const std::string_view primary_url, const std::string_view href) {
    if (href.starts_with("http://") || href.starts_with("https://")) return std::string{href};
    if (href.starts_with('/')) return "https://www.sec.gov" + std::string{href};
    return url_directory(primary_url) + std::string{href};
}

struct Event final {
    std::string article_id;
    std::string published_at_utc;
    std::string trading_date;
    std::string sector;
    std::string symbol;
    std::string primary_url;
    std::string content_hash;
};

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 4) {
            std::cout << "Usage: arrakis-build-sec-exhibit-manifest <sec_events.csv> <html_dir> <output.csv>\n";
            return 0;
        }
        std::ifstream events{argv[1]};
        if (!events) throw std::runtime_error{"Could not open SEC events"};
        const auto header = read_record(events);
        const auto article_id = column_index(header, "article_id");
        const auto published = column_index(header, "published_at_utc");
        const auto trading_date = column_index(header, "trading_date");
        const auto sector = column_index(header, "sector");
        const auto symbol = column_index(header, "symbol");
        const auto form = column_index(header, "form");
        const auto url = column_index(header, "url");
        const auto content_hash = column_index(header, "content_hash");
        std::ofstream output{argv[3]};
        if (!output) throw std::runtime_error{"Could not write SEC exhibit manifest"};
        output << "article_id,published_at_utc,trading_date,sector,symbol,primary_url,exhibit_url,exhibit_name,content_hash\n";

        std::size_t read = 0;
        std::size_t primary_documents = 0;
        std::size_t exhibit_links = 0;
        std::size_t missing = 0;
        std::unordered_set<std::string> seen;
        while (true) {
            const auto row = read_record(events);
            if (row.empty()) break;
            ++read;
            if (row.size() != header.size()) throw std::runtime_error{"Malformed SEC event row"};
            if (!row[form].starts_with("8-K")) continue;
            const auto html_path = std::filesystem::path{argv[2]} / (row[article_id] + ".html");
            if (!std::filesystem::exists(html_path)) {
                ++missing;
                continue;
            }
            std::ifstream html{html_path};
            std::ostringstream content;
            content << html.rdbuf();
            ++primary_documents;
            for (const auto& href : href_values(content.str())) {
                if (href.empty() || href.starts_with('#') || !looks_like_exhibit_99(href)) continue;
                const auto exhibit_url = resolve_url(row[url], href);
                const auto deduplication_key = row[article_id] + "\n" + exhibit_url;
                if (!seen.insert(deduplication_key).second) continue;
                const auto slash = href.rfind('/');
                const auto exhibit_name = slash == std::string::npos ? href : href.substr(slash + 1);
                output << csv_escape(row[article_id]) << ',' << csv_escape(row[published]) << ','
                       << row[trading_date] << ',' << row[sector] << ',' << row[symbol] << ','
                       << csv_escape(row[url]) << ',' << csv_escape(exhibit_url) << ','
                       << csv_escape(exhibit_name) << ',' << row[content_hash] << '\n';
                ++exhibit_links;
            }
        }
        std::ofstream manifest{std::string{argv[3]} + ".manifest.json"};
        if (!manifest) throw std::runtime_error{"Could not write SEC exhibit manifest metadata"};
        manifest << "{\n"
                 << "  \"source\": \"SEC 8-K primary documents and their linked Exhibit 99.x documents\",\n"
                 << "  \"rows_read\": " << read << ",\n"
                 << "  \"primary_documents\": " << primary_documents << ",\n"
                 << "  \"exhibit_links\": " << exhibit_links << ",\n"
                 << "  \"missing_primary_html\": " << missing << ",\n"
                 << "  \"link_policy\": \"only href attributes whose path identifies Exhibit 99.x; no filename guessing\"\n"
                 << "}\n";
        std::cout << "SEC Exhibit 99.x links: " << exhibit_links << " from " << primary_documents
                  << " primary documents\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-build-sec-exhibit-manifest: " << error.what() << '\n';
        return 1;
    }
}
