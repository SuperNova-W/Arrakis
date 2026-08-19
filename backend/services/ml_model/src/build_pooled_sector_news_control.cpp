#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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
    if (quoted) throw std::runtime_error{"CSV ended inside a quoted field"};
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

struct Aggregate final {
    std::size_t rows{};
    std::unordered_set<std::string> content_hashes;
    std::unordered_set<std::string> publishers;
};

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 5) {
            std::cout << "Usage: arrakis-build-pooled-sector-news-control <sector_articles.csv> "
                         "<pooled_market.csv> <output.csv> <to-date>\n";
            return 0;
        }
        std::ifstream news{argv[1]};
        if (!news) throw std::runtime_error{"Could not open sector news: " + std::string{argv[1]}};
        const auto news_header = read_record(news);
        const auto news_date = column_index(news_header, "trading_date");
        const auto news_sector = column_index(news_header, "sector");
        const auto news_hash = column_index(news_header, "content_hash");
        const auto news_publisher = column_index(news_header, "publisher");
        std::unordered_map<std::string, Aggregate> aggregates;
        for (auto row = read_record(news); !row.empty(); row = read_record(news)) {
            const auto max_index = std::max({news_date, news_sector, news_hash, news_publisher});
            if (row.size() <= max_index) throw std::runtime_error{"Malformed sector news row"};
            auto& aggregate = aggregates[row[news_date] + "|" + row[news_sector]];
            ++aggregate.rows;
            aggregate.content_hashes.insert(row[news_hash]);
            if (!row[news_publisher].empty()) aggregate.publishers.insert(row[news_publisher]);
        }

        std::ifstream market{argv[2]};
        if (!market) throw std::runtime_error{"Could not open pooled market data: " + std::string{argv[2]}};
        const auto market_header = read_record(market);
        const auto market_date = column_index(market_header, "date");
        const auto market_target = column_index(market_header, "target_next_close_up");
        std::ofstream output{argv[3]};
        if (!output) throw std::runtime_error{"Could not write news control dataset: " + std::string{argv[3]}};
        output << "date,article_count,news_coverage,article_novelty,publisher_breadth";
        for (std::size_t index = 0; index < market_header.size(); ++index) {
            if (index != market_date && index != market_target) output << ',' << market_header[index];
        }
        output << ',' << market_header[market_target] << '\n';

        std::size_t rows_written = 0;
        for (auto row = read_record(market); !row.empty(); row = read_record(market)) {
            if (row.size() != market_header.size()) throw std::runtime_error{"Malformed pooled market row"};
            const auto& key = row[market_date];
            const auto separator = key.find('|');
            if (separator == std::string::npos) throw std::runtime_error{"Pooled date lacks sector key"};
            const auto date = key.substr(0, separator);
            if (date > argv[4]) continue;
            const auto found = aggregates.find(key);
            const auto article_count = found == aggregates.end() ? 0.0 : static_cast<double>(found->second.rows);
            const auto unique_count = found == aggregates.end() ? 0.0 : static_cast<double>(found->second.content_hashes.size());
            const auto publisher_count = found == aggregates.end() ? 0.0 : static_cast<double>(found->second.publishers.size());
            output << csv_escape(key) << ',' << article_count << ',' << (article_count > 0.0 ? 1.0 : 0.0)
                   << ',' << (article_count > 0.0 ? unique_count / article_count : 0.0)
                   << ',' << publisher_count;
            for (std::size_t index = 0; index < market_header.size(); ++index) {
                if (index != market_date && index != market_target) output << ',' << row[index];
            }
            output << ',' << row[market_target] << '\n';
            ++rows_written;
        }

        std::ofstream manifest{std::string{argv[3]} + ".manifest.json"};
        if (!manifest) throw std::runtime_error{"Could not write news control manifest"};
        manifest << "{\n"
                 << "  \"input_news\": \"" << argv[1] << "\",\n"
                 << "  \"input_market\": \"" << argv[2] << "\",\n"
                 << "  \"rows_written\": " << rows_written << ",\n"
                 << "  \"feature_policy\": \"article_count, session coverage, unique-content ratio, publisher breadth; no FinBERT inference\",\n"
                 << "  \"timestamp_policy\": \"news rows were assigned to the first SPY session strictly after unverified FNSPID source_calendar_date\"\n"
                 << "}\n";
        std::cout << "Wrote " << rows_written << " pooled sector news-control rows\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-build-pooled-sector-news-control: " << error.what() << '\n';
        return 1;
    }
}
