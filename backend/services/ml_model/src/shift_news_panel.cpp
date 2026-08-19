#include <openssl/evp.h>

#include <array>
#include <algorithm>
#include <cctype>
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

[[nodiscard]] bool starts_with(const std::string_view value, const std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] std::string csv_escape(const std::string& value) {
    if (value.find_first_of(",\"\n\r") == std::string::npos) return value;
    std::string escaped{"\""};
    for (const char character : value) {
        escaped.push_back(character);
        if (character == '"') escaped.push_back('"');
    }
    escaped.push_back('"');
    return escaped;
}

[[nodiscard]] std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error{"Could not read output for hashing: " + path.string()};
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) throw std::runtime_error{"Could not allocate SHA-256 context"};
    const auto cleanup = [&]() { EVP_MD_CTX_free(context); };
    if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        cleanup();
        throw std::runtime_error{"Could not initialize SHA-256"};
    }
    std::array<char, 1U << 16U> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) != 1) {
            cleanup();
            throw std::runtime_error{"Could not update SHA-256"};
        }
    }
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context, digest, &digest_size) != 1) {
        cleanup();
        throw std::runtime_error{"Could not finalize SHA-256"};
    }
    cleanup();
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < digest_size; ++index) {
        output << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    return output.str();
}

[[nodiscard]] std::string sector_from_key(const std::string_view key) {
    const auto separator = key.find('|');
    if (separator == std::string_view::npos || separator == 0 || separator + 1U >= key.size()) {
        throw std::runtime_error{"Expected date key YYYY-MM-DD|SECTOR, got: " + std::string{key}};
    }
    const auto date = key.substr(0, separator);
    if (date.size() != 10U || date[4] != '-' || date[7] != '-') {
        throw std::runtime_error{"Expected date key YYYY-MM-DD|SECTOR, got: " + std::string{key}};
    }
    for (std::size_t index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U}) {
        if (!std::isdigit(static_cast<unsigned char>(date[index]))) {
            throw std::runtime_error{"Expected date key YYYY-MM-DD|SECTOR, got: " + std::string{key}};
        }
    }
    return std::string{key.substr(separator + 1U)};
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 3) {
            std::cout << "Usage: arrakis-shift-news-panel <input.csv> <output.csv>\n";
            return 0;
        }
        const auto input_path = std::filesystem::path{argv[1]};
        const auto output_path = std::filesystem::path{argv[2]};
        CsvReader reader{input_path};
        const auto header = reader.next();
        if (header.empty()) throw std::runtime_error{"Input CSV has no header"};
        const auto date_column = column_index(header, "date");
        std::vector<std::size_t> news_columns;
        for (std::size_t index = 0; index < header.size(); ++index) {
            if (starts_with(header[index], "embedding_") || starts_with(header[index], "finbert_")) {
                news_columns.push_back(index);
            }
        }
        if (news_columns.empty()) throw std::runtime_error{"Input CSV has no embedding_ or finbert_ columns"};

        std::vector<std::vector<std::string>> rows;
        while (true) {
            auto row = reader.next();
            if (row.empty()) break;
            if (row.size() != header.size()) throw std::runtime_error{"Malformed CSV row"};
            (void)sector_from_key(row[date_column]);
            rows.push_back(std::move(row));
        }
        if (rows.empty()) throw std::runtime_error{"Input CSV has no data rows"};

        std::map<std::string, std::vector<std::size_t>> by_sector;
        for (std::size_t index = 0; index < rows.size(); ++index) {
            by_sector[sector_from_key(rows[index][date_column])].push_back(index);
        }
        for (auto& [sector, indexes] : by_sector) {
            std::ranges::sort(indexes, [&rows, date_column](const std::size_t left, const std::size_t right) {
                return rows[left][date_column] < rows[right][date_column];
            });
            for (std::size_t position = 0; position + 1U < indexes.size(); ++position) {
                const auto current = indexes[position];
                const auto next = indexes[position + 1U];
                if (rows[current][date_column].substr(0, 10) >= rows[next][date_column].substr(0, 10)) {
                    throw std::runtime_error{"Sector rows are not strictly chronological: " + sector};
                }
                for (const auto column : news_columns) rows[current][column] = rows[next][column];
            }
            for (const auto column : news_columns) rows[indexes.back()][column] = "0";
        }

        if (!output_path.parent_path().empty()) std::filesystem::create_directories(output_path.parent_path());
        std::ofstream output{output_path};
        if (!output) throw std::runtime_error{"Could not write output: " + output_path.string()};
        for (std::size_t index = 0; index < header.size(); ++index) {
            if (index != 0U) output << ',';
            output << csv_escape(header[index]);
        }
        output << '\n';
        for (const auto& row : rows) {
            for (std::size_t index = 0; index < row.size(); ++index) {
                if (index != 0U) output << ',';
                output << csv_escape(row[index]);
            }
            output << '\n';
        }
        output.close();
        std::ofstream manifest{std::string{output_path} + ".manifest.json"};
        if (!manifest) throw std::runtime_error{"Could not write shift manifest"};
        manifest << "{\n"
                 << "  \"source_panel\": \"" << input_path.string() << "\",\n"
                 << "  \"output_panel\": \"" << output_path.string() << "\",\n"
                 << "  \"rows\": " << rows.size() << ",\n"
                 << "  \"columns\": " << header.size() << ",\n"
                 << "  \"news_columns\": " << news_columns.size() << ",\n"
                 << "  \"alignment\": \"row t receives news from the next market session within the same sector; final row is zero-filled\",\n"
                 << "  \"input_sha256\": \"" << sha256_file(input_path) << "\",\n"
                 << "  \"output_sha256\": \"" << sha256_file(output_path) << "\"\n"
                 << "}\n";
        std::cout << "Shifted " << news_columns.size() << " news columns across " << rows.size()
                  << " rows and " << by_sector.size() << " sectors\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-shift-news-panel: " << error.what() << '\n';
        return 1;
    }
}
