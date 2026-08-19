#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kEmbeddingDimensions = 768;

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

[[nodiscard]] std::string date_only(const std::string_view key) {
    if (key.size() < 10U) throw std::runtime_error{"Invalid panel date key: " + std::string{key}};
    return std::string{key.substr(0, 10)};
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 4 && argc != 5) {
            std::cout << "Usage: arrakis-merge-sec-news-panels <historical.csv> <later.csv> "
                         "<output.csv> [later-date-start]\n";
            return 0;
        }
        const auto historical_path = std::filesystem::path{argv[1]};
        const auto later_path = std::filesystem::path{argv[2]};
        const auto output_path = std::filesystem::path{argv[3]};
        const std::string later_date_start = argc == 5 ? std::string{argv[4]} : "2024-01-01";
        if (later_date_start.size() != 10U) throw std::invalid_argument{"Invalid later-date-start"};

        CsvReader historical{historical_path};
        CsvReader later{later_path};
        const auto historical_header = historical.next();
        const auto later_header = later.next();
        if (historical_header.empty() || later_header.empty()) {
            throw std::runtime_error{"One of the panel CSVs has no header"};
        }
        const auto historical_date = column_index(historical_header, "date");
        const auto later_date = column_index(later_header, "date");
        const auto historical_target = column_index(historical_header, "target_next_close_up");
        const auto later_target = column_index(later_header, "target_next_close_up");
        static_cast<void>(later_target);

        std::vector<std::size_t> historical_base_indices;
        std::vector<std::string> base_names;
        for (std::size_t index = 0; index < historical_header.size(); ++index) {
            const auto& name = historical_header[index];
            if (starts_with(name, "sec_embedding_") || name == "sec_embedding_article_count") continue;
            historical_base_indices.push_back(index);
            base_names.push_back(name);
            (void)column_index(later_header, name);
        }

        std::vector<std::size_t> historical_embedding_indices;
        historical_embedding_indices.reserve(kEmbeddingDimensions);
        std::vector<std::size_t> later_embedding_indices;
        later_embedding_indices.reserve(kEmbeddingDimensions);
        for (std::size_t index = 0; index < kEmbeddingDimensions; ++index) {
            historical_embedding_indices.push_back(
                column_index(historical_header, "sec_embedding_" + std::to_string(index))
            );
            later_embedding_indices.push_back(
                column_index(later_header, "embedding_" + std::to_string(index))
            );
        }
        const auto historical_count = column_index(historical_header, "sec_embedding_article_count");
        const auto later_count = column_index(later_header, "embedding_article_count");

        std::vector<std::size_t> later_extra_indices;
        std::vector<std::string> later_extra_names;
        for (std::size_t index = 0; index < later_header.size(); ++index) {
            const auto& name = later_header[index];
            if (starts_with(name, "finbert_") || name == "news_unique_symbol_count") {
                later_extra_indices.push_back(index);
                later_extra_names.push_back(name);
            }
        }

        if (!std::ranges::any_of(base_names, [](const auto& name) { return name == "date"; }) ||
            historical_target >= historical_header.size()) {
            throw std::runtime_error{"Panel base schema is invalid"};
        }
        if (!output_path.parent_path().empty()) std::filesystem::create_directories(output_path.parent_path());
        std::ofstream output{output_path};
        if (!output) throw std::runtime_error{"Could not write merged panel: " + output_path.string()};
        for (const auto& name : base_names) output << csv_escape(name) << ',';
        for (std::size_t index = 0; index < kEmbeddingDimensions; ++index) {
            output << "embedding_" << index << ',';
        }
        output << "embedding_article_count";
        for (const auto& name : later_extra_names) output << ',' << csv_escape(name);
        output << '\n';

        std::size_t rows = 0;
        std::size_t historical_rows = 0;
        std::size_t later_rows = 0;
        while (true) {
            const auto historical_row = historical.next();
            const auto later_row = later.next();
            if (historical_row.empty() && later_row.empty()) break;
            if (historical_row.empty() || later_row.empty()) throw std::runtime_error{"Panel row counts differ"};
            if (historical_row.size() != historical_header.size() || later_row.size() != later_header.size()) {
                throw std::runtime_error{"Malformed panel row"};
            }
            if (historical_row[historical_date] != later_row[later_date]) {
                throw std::runtime_error{
                    "Panel date keys differ: " + historical_row[historical_date] + " vs " + later_row[later_date]
                };
            }
            const auto use_later = date_only(historical_row[historical_date]) >= later_date_start;
            const auto& selected_news_row = use_later ? later_row : historical_row;
            const auto& selected_embedding_indices = use_later ? later_embedding_indices : historical_embedding_indices;
            const auto selected_count = use_later ? later_count : historical_count;
            for (std::size_t index = 0; index < historical_base_indices.size(); ++index) {
                if (index != 0U) output << ',';
                output << csv_escape(historical_row[historical_base_indices[index]]);
            }
            for (std::size_t index = 0; index < kEmbeddingDimensions; ++index) {
                output << ',' << csv_escape(selected_news_row[selected_embedding_indices[index]]);
            }
            output << ',' << csv_escape(selected_news_row[selected_count]);
            for (const auto index : later_extra_indices) {
                output << ',' << (use_later ? csv_escape(later_row[index]) : "0");
            }
            output << '\n';
            ++rows;
            if (use_later) ++later_rows;
            else ++historical_rows;
        }

        std::ofstream manifest{std::string{output_path} + ".manifest.json"};
        if (!manifest) throw std::runtime_error{"Could not write merged panel manifest"};
        manifest << "{\n"
                 << "  \"historical_panel\": \"" << historical_path.string() << "\",\n"
                 << "  \"later_panel\": \"" << later_path.string() << "\",\n"
                 << "  \"later_date_start\": \"" << later_date_start << "\",\n"
                 << "  \"rows\": " << rows << ",\n"
                 << "  \"historical_rows\": " << historical_rows << ",\n"
                 << "  \"later_rows\": " << later_rows << ",\n"
                 << "  \"embedding_dimensions\": " << kEmbeddingDimensions << ",\n"
                 << "  \"news_alignment\": \"each source panel supplies its own point-in-time feature row; market and target columns are copied from the historical panel\"\n"
                 << "}\n";
        std::cout << "Merged " << rows << " rows: " << historical_rows << " historical and " << later_rows
                  << " later rows\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-merge-sec-news-panels: " << error.what() << '\n';
        return 1;
    }
}
