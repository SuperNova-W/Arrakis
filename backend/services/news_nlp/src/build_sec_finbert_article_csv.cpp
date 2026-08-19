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

[[nodiscard]] std::string strip_html(const std::string_view html) {
    std::string text;
    text.reserve(std::min<std::size_t>(html.size(), 8192));
    std::size_t skipped_depth = 0;
    for (std::size_t index = 0; index < html.size();) {
        if (html[index] != '<') {
            if (skipped_depth == 0) {
                const auto character = static_cast<unsigned char>(html[index]);
                text.push_back(std::isspace(character) != 0 ? ' ' : static_cast<char>(character));
            }
            ++index;
            if (text.size() >= 8192) break;
            continue;
        }
        if (html.substr(index, 4) == "<!--") {
            const auto comment_end = html.find("-->", index + 4);
            if (comment_end != std::string_view::npos) {
                index = comment_end + 3;
                if (skipped_depth == 0) text.push_back(' ');
                continue;
            }
        }
        const auto tag_end = html.find('>', index + 1);
        if (tag_end == std::string_view::npos) break;
        std::string tag{html.substr(index, tag_end - index + 1)};
        std::ranges::transform(tag, tag.begin(), [](const char character) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        });
        const bool closing = tag.size() > 1 && tag[1] == '/';
        const bool self_closing = tag.size() > 2 && tag[tag.size() - 2] == '/';
        const bool hidden_open = !closing &&
            (tag.starts_with("<ix:header") ||
             (tag.starts_with("<div") && tag.find("display") != std::string::npos &&
              tag.find("none") != std::string::npos));
        const bool script_or_style_open = !closing &&
            (tag.starts_with("<script") || tag.starts_with("<style"));
        if (skipped_depth > 0) {
            if (closing) --skipped_depth;
            else if (!self_closing && !tag.starts_with("<!")) ++skipped_depth;
        } else if (hidden_open || script_or_style_open) {
            if (!self_closing) skipped_depth = 1;
        } else {
            text.push_back(' ');
        }
        index = tag_end + 1;
    }
    std::string collapsed;
    collapsed.reserve(text.size());
    bool previous_space = true;
    for (const auto character : text) {
        if (character == ' ') {
            if (!previous_space) collapsed.push_back(character);
            previous_space = true;
        } else {
            collapsed.push_back(character);
            previous_space = false;
        }
    }
    while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
    return collapsed;
}

[[nodiscard]] std::string semantic_filing_text(const std::string& body) {
    std::string lower = body;
    std::ranges::transform(lower, lower.begin(), [](const char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    });
    std::vector<std::size_t> item_starts;
    for (std::size_t position = lower.find("item "); position != std::string::npos;
         position = lower.find("item ", position + 5)) {
        const bool word_boundary = position == 0 || !std::isalnum(static_cast<unsigned char>(lower[position - 1]));
        const auto number = position + 5;
        if (word_boundary && number < lower.size() && std::isdigit(static_cast<unsigned char>(lower[number])) != 0) {
            item_starts.push_back(position);
        }
    }
    if (item_starts.empty()) return body.substr(0, 8192);
    std::string selected;
    selected.reserve(std::min<std::size_t>(body.size(), 8192));
    for (std::size_t index = 0; index < item_starts.size(); ++index) {
        const auto begin = item_starts[index];
        const auto end = index + 1 < item_starts.size() ? item_starts[index + 1] : body.size();
        const auto section = lower.substr(begin, std::min<std::size_t>(end - begin, 16));
        if (section.starts_with("item 9.01")) continue;
        if (!selected.empty()) selected.push_back(' ');
        selected.append(body, begin, end - begin);
        if (selected.size() >= 8192) break;
    }
    return selected.empty() ? body.substr(0, 8192) : selected.substr(0, 8192);
}

[[nodiscard]] std::string finbert_timestamp(const std::string& value) {
    if (value.size() < 19) throw std::runtime_error{"SEC timestamp is too short"};
    return value.substr(0, 10) + " " + value.substr(11, 8);
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 4 && argc != 5) {
            std::cout << "Usage: arrakis-build-sec-finbert-article-csv <sec_events.csv> <html_dir> "
                         "<output.csv> [all-supported]\n";
            return 0;
        }
        const bool include_all_supported = argc == 5 && std::string_view{argv[4]} == "all-supported";
        if (argc == 5 && !include_all_supported) throw std::invalid_argument{"Unknown SEC article policy"};
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
        if (!output) throw std::runtime_error{"Could not write SEC FinBERT article CSV"};
        output << "article_id,published_at_utc,trading_date,sector,symbol,title,summary,url,publisher,content_hash\n";
        std::size_t read = 0;
        std::size_t written = 0;
        std::size_t missing = 0;
        while (true) {
            const auto row = read_record(events);
            if (row.empty()) break;
            ++read;
            if (row.size() != header.size()) throw std::runtime_error{"Malformed SEC event row"};
            if (!include_all_supported && !row[form].starts_with("8-K")) continue;
            const auto html_path = std::filesystem::path{argv[2]} / (row[article_id] + ".html");
            if (!std::filesystem::exists(html_path)) {
                ++missing;
                continue;
            }
            std::ifstream html{html_path};
            std::ostringstream content;
            content << html.rdbuf();
            const auto text = semantic_filing_text(strip_html(content.str()));
            if (text.empty()) {
                ++missing;
                continue;
            }
            output << csv_escape(row[article_id]) << ',' << finbert_timestamp(row[published]) << ','
                   << row[trading_date] << ',' << row[sector] << ',' << row[symbol] << ','
                   << csv_escape(text) << ",," << csv_escape(row[url]) << ",SEC," << row[content_hash] << '\n';
            ++written;
        }
        std::ofstream manifest{std::string{argv[3]} + ".manifest.json"};
        if (!manifest) throw std::runtime_error{"Could not write SEC FinBERT article manifest"};
        manifest << "{\n"
                 << "  \"source\": \"SEC 8-K primary documents with exact acceptance timestamps\",\n"
                 << "  \"rows_read\": " << read << ",\n"
                 << "  \"rows_written\": " << written << ",\n"
                 << "  \"missing_html_or_text\": " << missing << ",\n"
                 << "  \"text_policy\": \"visible text, hidden XBRL/header/script/style removed, Item 9.01 omitted, 8192-character cap\",\n"
                 << "  \"form_policy\": \""
                 << (include_all_supported ? "all supported SEC forms" : "8-K and 8-K/A only") << "\",\n"
                 << "  \"timestamp_policy\": \"SEC acceptance UTC formatted for the FinBERT cache adapter\"\n}\n";
        std::cout << "SEC FinBERT article rows: " << written << " from " << read << " events\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-build-sec-finbert-article-csv: " << error.what() << '\n';
        return 1;
    }
}
