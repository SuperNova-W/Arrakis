#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <ranges>
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
    std::size_t articles{};
    std::unordered_set<std::string> issuers;
    std::size_t eight_k{};
    std::size_t ten_q{};
    std::size_t ten_k{};
    std::size_t other{};
    std::size_t sentiment_rows{};
    double sentiment_sum{};
};

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 6) {
            std::cout << "Usage: arrakis-build-pooled-sector-sec-finbert-control <sec_events.csv> "
                         "<sentiment.csv> <pooled_market.csv> <output.csv> <to-date>\n";
            return 0;
        }

        std::ifstream events{argv[1]};
        if (!events) throw std::runtime_error{"Could not open SEC filing events"};
        const auto event_header = read_record(events);
        const auto event_id = column_index(event_header, "article_id");
        const auto event_date = column_index(event_header, "trading_date");
        const auto event_sector = column_index(event_header, "sector");
        const auto event_symbol = column_index(event_header, "symbol");
        const auto event_form = column_index(event_header, "form");
        std::unordered_map<std::string, std::string> event_keys;
        std::unordered_map<std::string, Aggregate> aggregate;
        while (true) {
            const auto row = read_record(events);
            if (row.empty()) break;
            if (row.size() != event_header.size()) throw std::runtime_error{"Malformed SEC event row"};
            const auto key = row[event_date] + "|" + row[event_sector];
            if (!event_keys.emplace(row[event_id], key).second) {
                throw std::runtime_error{"Duplicate SEC article_id: " + row[event_id]};
            }
            auto& item = aggregate[key];
            ++item.articles;
            item.issuers.insert(row[event_symbol]);
            if (row[event_form].starts_with("8-K")) ++item.eight_k;
            else if (row[event_form].starts_with("10-Q")) ++item.ten_q;
            else if (row[event_form].starts_with("10-K")) ++item.ten_k;
            else ++item.other;
        }

        std::ifstream sentiment{argv[2]};
        if (!sentiment) throw std::runtime_error{"Could not open SEC FinBERT sentiment cache"};
        const auto sentiment_header = read_record(sentiment);
        const auto sentiment_id = column_index(sentiment_header, "article_id");
        const auto sentiment_date = column_index(sentiment_header, "trading_date");
        const auto sentiment_sector = column_index(sentiment_header, "sector");
        const auto sentiment_score = column_index(sentiment_header, "sentiment_score");
        std::unordered_set<std::string> seen_sentiment;
        std::size_t sentiment_rows = 0;
        while (true) {
            const auto row = read_record(sentiment);
            if (row.empty()) break;
            if (row.size() != sentiment_header.size()) throw std::runtime_error{"Malformed FinBERT sentiment row"};
            const auto event = event_keys.find(row[sentiment_id]);
            if (event == event_keys.end()) throw std::runtime_error{"Sentiment row has unknown article_id"};
            const auto expected_key = row[sentiment_date] + "|" + row[sentiment_sector];
            if (event->second != expected_key) throw std::runtime_error{"Sentiment row disagrees with SEC event key"};
            if (!seen_sentiment.insert(row[sentiment_id]).second) throw std::runtime_error{"Duplicate FinBERT sentiment row"};
            auto& item = aggregate.at(expected_key);
            item.sentiment_sum += std::stod(row[sentiment_score]);
            ++item.sentiment_rows;
            ++sentiment_rows;
        }

        std::ifstream market{argv[3]};
        if (!market) throw std::runtime_error{"Could not open pooled market dataset"};
        const auto market_header = read_record(market);
        const auto market_date = column_index(market_header, "date");
        const auto market_target = column_index(market_header, "target_next_close_up");
        std::ofstream output{argv[4]};
        if (!output) throw std::runtime_error{"Could not write SEC FinBERT control dataset"};
        output << std::setprecision(17)
               << "date,sec_article_count,sec_issuer_breadth,sec_8k_count,sec_10q_count,sec_10k_count,sec_other_count,sec_finbert_sentiment_mean";
        for (std::size_t index = 0; index < market_header.size(); ++index) {
            if (index != market_date && index != market_target) output << ',' << market_header[index];
        }
        output << ',' << market_header[market_target] << '\n';

        std::size_t rows_written = 0;
        while (true) {
            const auto row = read_record(market);
            if (row.empty()) break;
            if (row.size() != market_header.size()) throw std::runtime_error{"Malformed pooled market row"};
            const auto separator = row[market_date].find('|');
            if (separator == std::string::npos) throw std::runtime_error{"Pooled market date lacks sector"};
            if (row[market_date].substr(0, separator) > argv[5]) continue;
            const auto found = aggregate.find(row[market_date]);
            const Aggregate empty{};
            const auto& item = found == aggregate.end() ? empty : found->second;
            const auto mean_sentiment = item.sentiment_rows == 0
                ? 0.0
                : item.sentiment_sum / static_cast<double>(item.sentiment_rows);
            output << csv_escape(row[market_date]) << ',' << item.articles << ',' << item.issuers.size() << ','
                   << item.eight_k << ',' << item.ten_q << ',' << item.ten_k << ',' << item.other << ','
                   << mean_sentiment;
            for (std::size_t index = 0; index < market_header.size(); ++index) {
                if (index != market_date && index != market_target) output << ',' << row[index];
            }
            output << ',' << row[market_target] << '\n';
            ++rows_written;
        }

        std::ofstream manifest{std::string{argv[4]} + ".manifest.json"};
        if (!manifest) throw std::runtime_error{"Could not write SEC FinBERT control manifest"};
        manifest << "{\n"
                 << "  \"input_events\": \"" << argv[1] << "\",\n"
                 << "  \"input_sentiment\": \"" << argv[2] << "\",\n"
                 << "  \"input_market\": \"" << argv[3] << "\",\n"
                 << "  \"rows_written\": " << rows_written << ",\n"
                 << "  \"sentiment_rows_joined\": " << sentiment_rows << ",\n"
                 << "  \"features\": [\"sec_article_count\",\"sec_issuer_breadth\",\"sec_8k_count\",\"sec_10q_count\",\"sec_10k_count\",\"sec_other_count\",\"sec_finbert_sentiment_mean\"],\n"
                 << "  \"missing_sentiment_value\": 0.0,\n"
                 << "  \"timestamp_policy\": \"SEC acceptance UTC; assigned to first SPY session strictly after acceptance calendar date\"\n}\n";
        std::cout << "Wrote " << rows_written << " pooled SEC+FinBERT rows; joined "
                  << sentiment_rows << " sentiment rows\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-build-pooled-sector-sec-finbert-control: " << error.what() << '\n';
        return 1;
    }
}
