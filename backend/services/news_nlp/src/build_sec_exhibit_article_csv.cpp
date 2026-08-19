#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
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
    if (quoted) throw std::runtime_error{"SEC exhibit manifest ended inside a quoted field"};
    if (!field.empty() || !fields.empty()) fields.push_back(field);
    return fields;
}

[[nodiscard]] std::size_t column_index(
    const std::vector<std::string>& header,
    const std::string_view name
) {
    const auto found = std::ranges::find(header, name);
    if (found == header.end()) throw std::runtime_error{"Missing SEC exhibit column: " + std::string{name}};
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

[[nodiscard]] std::string lower_copy(std::string value) {
    std::ranges::transform(value, value.begin(), [](const char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    });
    return value;
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
        const auto comment_end = html.find("-->", index + 4);
        if (html.substr(index, 4) == "<!--" && comment_end != std::string_view::npos) {
            index = comment_end + 3;
            if (skipped_depth == 0) text.push_back(' ');
            continue;
        }
        const auto tag_end = html.find('>', index + 1);
        if (tag_end == std::string_view::npos) break;
        std::string tag{html.substr(index, tag_end - index + 1)};
        const auto lower_tag = lower_copy(tag);
        const bool closing = lower_tag.size() > 1 && lower_tag[1] == '/';
        const bool self_closing = lower_tag.size() > 2 && lower_tag[lower_tag.size() - 2] == '/';
        const bool hidden_open = !closing &&
            (lower_tag.starts_with("<ix:header") ||
             (lower_tag.starts_with("<div") && lower_tag.find("display") != std::string::npos &&
              lower_tag.find("none") != std::string::npos));
        const bool script_or_style_open = !closing &&
            (lower_tag.starts_with("<script") || lower_tag.starts_with("<style"));
        if (skipped_depth > 0) {
            if (closing) --skipped_depth;
            else if (!self_closing && !lower_tag.starts_with("<!")) ++skipped_depth;
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

[[nodiscard]] bool has_meaningful_text(const std::string_view text) {
    return std::ranges::any_of(text, [](const char character) {
        return std::isalnum(static_cast<unsigned char>(character)) != 0;
    });
}

[[nodiscard]] std::string finbert_timestamp(const std::string& value) {
    if (value.size() < 19) throw std::runtime_error{"SEC timestamp is too short"};
    return value.substr(0, 10) + " " + value.substr(11, 8);
}

[[nodiscard]] std::string local_name(const std::string_view article_id, const std::string_view exhibit_name) {
    std::string result{article_id};
    std::ranges::replace(result, ':', '_');
    result += "__";
    for (const auto character : exhibit_name) result.push_back(character == '/' ? '_' : character);
    return result;
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 4) {
            std::cout << "Usage: arrakis-build-sec-exhibit-article-csv <exhibit_manifest.csv> "
                         "<exhibit_dir> <output.csv>\n";
            return 0;
        }
        std::ifstream manifest{argv[1]};
        if (!manifest) throw std::runtime_error{"Could not open SEC exhibit manifest"};
        const auto header = read_record(manifest);
        const auto article_id = column_index(header, "article_id");
        const auto published = column_index(header, "published_at_utc");
        const auto trading_date = column_index(header, "trading_date");
        const auto sector = column_index(header, "sector");
        const auto symbol = column_index(header, "symbol");
        const auto exhibit_url = column_index(header, "exhibit_url");
        const auto exhibit_name = column_index(header, "exhibit_name");
        std::ofstream output{argv[3]};
        if (!output) throw std::runtime_error{"Could not write SEC exhibit article CSV"};
        output << "article_id,published_at_utc,trading_date,sector,symbol,title,summary,url,publisher,content_hash\n";

        std::size_t read = 0;
        std::size_t written = 0;
        std::size_t missing = 0;
        std::size_t empty = 0;
        while (true) {
            const auto row = read_record(manifest);
            if (row.empty()) break;
            ++read;
            if (row.size() != header.size()) throw std::runtime_error{"Malformed SEC exhibit row"};
            const auto path = std::filesystem::path{argv[2]} / local_name(row[article_id], row[exhibit_name]);
            if (!std::filesystem::exists(path)) {
                ++missing;
                continue;
            }
            std::ifstream html{path};
            std::ostringstream content;
            content << html.rdbuf();
            const auto text = strip_html(content.str());
            if (!has_meaningful_text(text)) {
                ++empty;
                continue;
            }
            const auto unique_id = "sec-exhibit:" + sha256(row[exhibit_url]);
            output << csv_escape(unique_id) << ',' << finbert_timestamp(row[published]) << ','
                   << row[trading_date] << ',' << row[sector] << ',' << row[symbol] << ','
                   << csv_escape(row[exhibit_name]) << ",," << csv_escape(row[exhibit_url])
                   << ",SEC," << sha256(text) << '\n';
            ++written;
        }
        std::ofstream output_manifest{std::string{argv[3]} + ".manifest.json"};
        if (!output_manifest) throw std::runtime_error{"Could not write SEC exhibit article metadata"};
        output_manifest << "{\n"
                        << "  \"source\": \"SEC linked Exhibit 99.x documents\",\n"
                        << "  \"rows_read\": " << read << ",\n"
                        << "  \"rows_written\": " << written << ",\n"
                        << "  \"missing_download\": " << missing << ",\n"
                        << "  \"empty_or_image_only\": " << empty << ",\n"
                        << "  \"text_policy\": \"visible HTML text, hidden XBRL/header/script/style removed, 8192-character cap\",\n"
                        << "  \"article_id_policy\": \"sha256 of canonical exhibit URL\",\n"
                        << "  \"timestamp_policy\": \"SEC acceptance UTC and prior-session trading_date inherited from event manifest\"\n"
                        << "}\n";
        std::cout << "SEC exhibit article rows: " << written << " from " << read
                  << " linked exhibits\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-build-sec-exhibit-article-csv: " << error.what() << '\n';
        return 1;
    }
}
