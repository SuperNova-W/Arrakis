#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::size_t kEmbeddingDimensions = 768;
constexpr std::size_t kRetainedDimensions = kEmbeddingDimensions;
constexpr std::size_t kSentimentDimensions = 4;

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
    if (found == header.end()) throw std::runtime_error{"Missing CSV column: " + std::string{name}};
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
    std::array<double, kRetainedDimensions> sums{};
    std::array<double, kSentimentDimensions> sentiment_sums{};
    double sentiment_score_square_sum{};
    double sentiment_score_min{};
    double sentiment_score_max{};
    bool has_sentiment_score{};
    std::unordered_set<std::string> symbols;
    std::size_t articles{};
};

struct EmbeddingIndex final {
    std::unordered_map<std::string, std::size_t> row_by_input_hash;
    std::vector<float> values;
    std::vector<float> sentiment_values;
};

[[nodiscard]] EmbeddingIndex load_embedding_index(
    const std::filesystem::path& index_path,
    const std::filesystem::path& embedding_path,
    const std::filesystem::path& sentiment_path
) {
    std::ifstream index{index_path};
    if (!index) throw std::runtime_error{"Could not open embedding index: " + index_path.string()};
    const auto header = read_record(index);
    const auto row_index = column_index(header, "row");
    // New caches use input_hash; accept the historical content_hash label while
    // reading older artifacts so resumable research runs remain reproducible.
    const auto input_hash_name = std::ranges::find(header, "input_hash");
    const auto content_hash_name = std::ranges::find(header, "content_hash");
    if (input_hash_name == header.end() && content_hash_name == header.end()) {
        throw std::runtime_error{"Embedding index is missing input_hash"};
    }
    const auto input_hash = static_cast<std::size_t>(std::distance(
        header.begin(), input_hash_name != header.end() ? input_hash_name : content_hash_name));
    EmbeddingIndex result;
    while (true) {
        const auto row = read_record(index);
        if (row.empty()) break;
        if (row.size() != header.size()) throw std::runtime_error{"Malformed embedding index row"};
        result.row_by_input_hash.emplace(row[input_hash], static_cast<std::size_t>(std::stoull(row[row_index])));
    }
    std::ifstream embeddings{embedding_path, std::ios::binary};
    if (!embeddings) throw std::runtime_error{"Could not open embedding matrix: " + embedding_path.string()};
    const auto byte_count = std::filesystem::file_size(embedding_path);
    const auto expected_width = kEmbeddingDimensions * sizeof(float);
    if (byte_count % expected_width != 0) throw std::runtime_error{"Embedding matrix has an invalid byte length"};
    const auto value_count = static_cast<std::size_t>(byte_count / sizeof(float));
    result.values.resize(value_count);
    embeddings.read(reinterpret_cast<char*>(result.values.data()), static_cast<std::streamsize>(byte_count));
    if (!embeddings) throw std::runtime_error{"Could not read embedding matrix"};
    for (const auto& [_, row] : result.row_by_input_hash) {
        if (row >= value_count / kEmbeddingDimensions) throw std::runtime_error{"Embedding index row is out of range"};
    }
    if (!sentiment_path.empty()) {
        const auto sentiment_bytes = std::filesystem::file_size(sentiment_path);
        const auto expected_sentiment_width = kSentimentDimensions * sizeof(float);
        if (sentiment_bytes % expected_sentiment_width != 0) throw std::runtime_error{"Sentiment matrix has an invalid byte length"};
        const auto sentiment_count = static_cast<std::size_t>(sentiment_bytes / sizeof(float));
        result.sentiment_values.resize(sentiment_count);
        std::ifstream sentiments{sentiment_path, std::ios::binary};
        if (!sentiments) throw std::runtime_error{"Could not open sentiment matrix: " + sentiment_path.string()};
        sentiments.read(reinterpret_cast<char*>(result.sentiment_values.data()), static_cast<std::streamsize>(sentiment_bytes));
        if (!sentiments) throw std::runtime_error{"Could not read sentiment matrix"};
        for (const auto& [_, row] : result.row_by_input_hash) {
            if (row >= sentiment_count / kSentimentDimensions) throw std::runtime_error{"Sentiment index row is out of range"};
        }
    }
    return result;
}

[[nodiscard]] std::map<std::string, Aggregate> load_aggregates(
    const std::filesystem::path& occurrences_path,
    const EmbeddingIndex& embeddings
) {
    std::ifstream occurrences{occurrences_path};
    if (!occurrences) throw std::runtime_error{"Could not open embedding occurrences: " + occurrences_path.string()};
    const auto header = read_record(occurrences);
    const auto input_hash = column_index(header, "input_hash");
    const auto content_hash = column_index(header, "content_hash");
    const auto trading_date = column_index(header, "trading_date");
    const auto sector = column_index(header, "sector");
    const auto symbol = column_index(header, "symbol");
    std::map<std::string, Aggregate> result;
    std::unordered_set<std::string> seen_articles;
    std::size_t missing_embeddings = 0;
    while (true) {
        const auto row = read_record(occurrences);
        if (row.empty()) break;
        if (row.size() != header.size()) throw std::runtime_error{"Malformed embedding occurrence row"};
        const auto found = embeddings.row_by_input_hash.find(row[input_hash]);
        if (found == embeddings.row_by_input_hash.end()) {
            ++missing_embeddings;
            continue;
        }
        auto& aggregate = result[row[trading_date] + "|" + row[sector]];
        aggregate.symbols.insert(row[symbol]);
        // One article can mention several holdings in the same ETF.  Count
        // its FinBERT/news signal once per sector and assigned session so
        // symbol fan-out cannot overweight a single publication.
        const auto article_key = row[trading_date] + "|" + row[sector] + "|" + row[content_hash];
        if (!seen_articles.insert(article_key).second) continue;
        const auto offset = found->second * kEmbeddingDimensions;
        for (std::size_t dimension = 0; dimension < kRetainedDimensions; ++dimension) {
            aggregate.sums[dimension] += static_cast<double>(embeddings.values[offset + dimension]);
        }
        if (!embeddings.sentiment_values.empty()) {
            const auto sentiment_offset = found->second * kSentimentDimensions;
            for (std::size_t dimension = 0; dimension < kSentimentDimensions; ++dimension) {
                aggregate.sentiment_sums[dimension] += static_cast<double>(embeddings.sentiment_values[sentiment_offset + dimension]);
            }
            const auto sentiment_score = static_cast<double>(embeddings.sentiment_values[sentiment_offset + 3]);
            aggregate.sentiment_score_square_sum += sentiment_score * sentiment_score;
            if (!aggregate.has_sentiment_score) {
                aggregate.sentiment_score_min = sentiment_score;
                aggregate.sentiment_score_max = sentiment_score;
                aggregate.has_sentiment_score = true;
            } else {
                aggregate.sentiment_score_min = std::min(aggregate.sentiment_score_min, sentiment_score);
                aggregate.sentiment_score_max = std::max(aggregate.sentiment_score_max, sentiment_score);
            }
        }
        ++aggregate.articles;
    }
    if (result.empty()) throw std::runtime_error{"No sector/session embedding aggregates were produced"};
    std::cerr << "Embedding aggregates=" << result.size() << ", missing_occurrence_embeddings="
              << missing_embeddings << '\n';
    return result;
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 6 && argc != 7) {
            std::cout << "Usage: arrakis-build-pooled-sector-sec-embedding-control <pooled_market.csv> "
                         "<occurrences.csv> <embedding_index.csv> <embeddings.f32> <output.csv> [sentiment.f32]\n";
            return 0;
        }
        const auto market_path = std::filesystem::path{argv[1]};
        const auto occurrences_path = std::filesystem::path{argv[2]};
        const auto index_path = std::filesystem::path{argv[3]};
        const auto embedding_path = std::filesystem::path{argv[4]};
        const auto output_path = std::filesystem::path{argv[5]};
        const auto sentiment_path = argc == 7 ? std::filesystem::path{argv[6]} : std::filesystem::path{};
        if (output_path.empty()) throw std::invalid_argument{"Output path is required"};
        const auto embeddings = load_embedding_index(index_path, embedding_path, sentiment_path);
        const auto aggregates = load_aggregates(occurrences_path, embeddings);
        std::ifstream market{market_path};
        if (!market) throw std::runtime_error{"Could not open pooled market dataset: " + market_path.string()};
        const auto header = read_record(market);
        const auto date = column_index(header, "date");
        if (!output_path.parent_path().empty()) std::filesystem::create_directories(output_path.parent_path());
        std::ofstream output{output_path};
        if (!output) throw std::runtime_error{"Could not write pooled embedding dataset"};
        for (std::size_t index = 0; index < header.size(); ++index) output << (index == 0 ? "" : ",") << header[index];
        for (std::size_t dimension = 0; dimension < kRetainedDimensions; ++dimension) {
            output << ",embedding_" << dimension;
        }
        output << ",embedding_article_count";
        if (!embeddings.sentiment_values.empty()) {
            output << ",finbert_positive_probability,finbert_negative_probability,finbert_neutral_probability,finbert_sentiment_score,finbert_sentiment_score_std,finbert_sentiment_score_min,finbert_sentiment_score_max,news_unique_symbol_count";
        }
        output << '\n';
        std::size_t rows = 0;
        std::size_t rows_with_embeddings = 0;
        while (true) {
            const auto row = read_record(market);
            if (row.empty()) break;
            if (row.size() != header.size()) throw std::runtime_error{"Malformed pooled market row"};
            for (std::size_t index = 0; index < row.size(); ++index) output << (index == 0 ? "" : ",") << csv_escape(row[index]);
            const auto found = aggregates.find(row[date]);
            if (found == aggregates.end()) {
                for (std::size_t dimension = 0; dimension < kRetainedDimensions; ++dimension) output << ",0";
                output << ",0";
                if (!embeddings.sentiment_values.empty()) output << ",0,0,0,0,0,0,0,0";
                output << '\n';
            } else {
                ++rows_with_embeddings;
                const auto denominator = static_cast<double>(found->second.articles);
                for (const auto sum : found->second.sums) output << ',' << sum / denominator;
                output << ',' << found->second.articles;
                if (!embeddings.sentiment_values.empty()) {
                    for (const auto sum : found->second.sentiment_sums) output << ',' << sum / denominator;
                    const auto sentiment_mean = found->second.sentiment_sums[3] / denominator;
                    const auto sentiment_variance = std::max(
                        0.0, found->second.sentiment_score_square_sum / denominator - sentiment_mean * sentiment_mean);
                    output << ',' << std::sqrt(sentiment_variance)
                           << ',' << found->second.sentiment_score_min
                           << ',' << found->second.sentiment_score_max
                           << ',' << found->second.symbols.size();
                }
                output << '\n';
            }
            ++rows;
        }
        std::ofstream manifest{std::string{output_path} + ".manifest.json"};
        if (!manifest) throw std::runtime_error{"Could not write pooled embedding manifest"};
        manifest << "{\n"
                 << "  \"source\": \"point-in-time sector article occurrences plus frozen FinBERT pooled embeddings\",\n"
                 << "  \"embedding_dimensions\": " << kEmbeddingDimensions << ",\n"
                 << "  \"retained_dimensions\": " << kRetainedDimensions << ",\n"
                 << "  \"finbert_sentiment_features\": " << (!embeddings.sentiment_values.empty() ? "true" : "false") << ",\n"
                 << "  \"rows_written\": " << rows << ",\n"
                 << "  \"rows_with_embeddings\": " << rows_with_embeddings << ",\n"
                 << "  \"aggregation\": \"mean FinBERT features per unique content_hash, sector, and assigned trading session; coverage and sentiment dispersion are computed from point-in-time occurrences; missing sessions are zero-filled\",\n"
                 << "  \"occurrences_input\": \"" << occurrences_path.string() << "\",\n"
                 << "  \"timestamp_policy\": \"the upstream article adapter's event-to-feature-session assignment is encoded in occurrence trading_date\"\n}\n";
        std::cout << "Wrote " << rows << " pooled SEC embedding rows; covered " << rows_with_embeddings << " rows\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-build-pooled-sector-sec-embedding-control: " << error.what() << '\n';
        return 1;
    }
}
