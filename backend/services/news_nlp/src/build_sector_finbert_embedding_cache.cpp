#include "arrakis/news/finbert.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
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

constexpr std::size_t kEmbeddingDimensions = 768;
constexpr std::size_t kSentimentDimensions = 4;
constexpr std::size_t kDefaultBatchSize = 64;
constexpr std::string_view kInputPolicyVersion{"sector_headline_v2_exact_token_inputs_max64_pti_v2"};
constexpr std::string_view kModelSha256{
    "c7f8304257b2a587d9d9b348410b3809cc9403da909cc0331a63294426e4205a"};
constexpr std::string_view kVocabSha256{
    "07eced375cec144d27c900241f3e339dec958f92fddbc551f295c992038a3"};

struct Article final {
    std::string hash;
    std::string input_hash;
    std::string published_at_utc;
    std::string trading_date;
    std::string sector;
    std::string symbol;
    std::string headline;
};

struct ScanResult final {
    std::vector<Article> selected;
    std::vector<Article> occurrences;
    std::size_t rows_read{};
    std::size_t rows_with_valid_timestamp{};
    std::size_t unique_content_hashes{};
    std::size_t unique_model_inputs{};
    std::set<std::string> eligible_dates;
    std::set<std::string> eligible_sectors;
};

struct ShardConfig final {
    std::size_t index{};
    std::size_t count{1};
};

[[nodiscard]] ShardConfig configured_shard() {
    const auto* count_value = std::getenv("ARRAKIS_FINBERT_CACHE_SHARD_COUNT");
    const auto* index_value = std::getenv("ARRAKIS_FINBERT_CACHE_SHARD_INDEX");
    if (count_value == nullptr && index_value == nullptr) return {};
    if (count_value == nullptr || index_value == nullptr) {
        throw std::invalid_argument{
            "ARRAKIS_FINBERT_CACHE_SHARD_COUNT and _INDEX must be provided together"};
    }
    const auto count = static_cast<std::size_t>(std::stoull(count_value));
    const auto index = static_cast<std::size_t>(std::stoull(index_value));
    if (count < 2 || index >= count) throw std::invalid_argument{"Invalid cache shard configuration"};
    return ShardConfig{.index = index, .count = count};
}

[[nodiscard]] bool belongs_to_shard(const std::string_view input_hash, const ShardConfig shard) {
    if (shard.count == 1) return true;
    const auto high_nibble = static_cast<unsigned int>(std::stoul(
        std::string{input_hash.substr(0, 2)}, nullptr, 16
    ));
    return high_nibble % shard.count == shard.index;
}

[[nodiscard]] std::size_t configured_batch_size() {
    const auto* value = std::getenv("ARRAKIS_FINBERT_CACHE_BATCH_SIZE");
    if (value == nullptr || *value == '\0') return kDefaultBatchSize;
    const auto parsed = static_cast<std::size_t>(std::stoull(value));
    if (parsed == 0 || parsed > 1024) {
        throw std::invalid_argument{"ARRAKIS_FINBERT_CACHE_BATCH_SIZE must be in [1,1024]"};
    }
    return parsed;
}

[[nodiscard]] std::vector<std::string> read_record(std::istream& input) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    char character = 0;
    while (input.get(character)) {
        if (character == '"') {
            if (quoted && input.peek() == '"') {
                input.get(character);
                field.push_back('"');
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
    if (quoted) throw std::runtime_error{"Sector news CSV ended inside a quoted field"};
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

[[nodiscard]] std::int64_t parse_utc_ms(const std::string_view value) {
    if (value.size() < 19) throw std::invalid_argument{"Invalid UTC timestamp"};
    std::tm parsed{};
    std::istringstream input{std::string{value.substr(0, 19)}};
    input >> std::get_time(&parsed, "%Y-%m-%d %H:%M:%S");
    if (input.fail()) throw std::invalid_argument{"Invalid UTC timestamp"};
    return static_cast<std::int64_t>(timegm(&parsed)) * 1000;
}

[[nodiscard]] ScanResult scan_articles(
    const std::filesystem::path& input_path,
    const std::string_view from_date,
    const std::string_view to_date,
    const std::size_t limit,
    const ShardConfig shard,
    const arrakis::news::FinbertSession& tokenizer
) {
    std::ifstream input{input_path};
    if (!input) throw std::runtime_error{"Could not open sector news: " + input_path.string()};
    const auto header = read_record(input);
    const auto published_index = column_index(header, "published_at_utc");
    const auto date_index = column_index(header, "trading_date");
    const auto sector_index = column_index(header, "sector");
    const auto symbol_index = column_index(header, "symbol");
    const auto title_index = column_index(header, "title");
    const auto hash_index = column_index(header, "content_hash");

    ScanResult result;
    std::unordered_set<std::string> seen_hashes;
    std::unordered_set<std::string> seen_inputs;
    while (true) {
        const auto fields = read_record(input);
        if (fields.empty()) break;
        ++result.rows_read;
        const auto max_index = std::max(
            {published_index, date_index, sector_index, symbol_index, title_index, hash_index}
        );
        if (fields.size() <= max_index) continue;
        const auto& date = fields[date_index];
        const auto& sector = fields[sector_index];
        const auto& symbol = fields[symbol_index];
        const auto& headline = fields[title_index];
        const auto& hash = fields[hash_index];
        if (date < from_date || date > to_date || sector.empty() || symbol.empty() ||
            headline.empty() || hash.empty()) {
            continue;
        }
        try {
            static_cast<void>(parse_utc_ms(fields[published_index]));
        } catch (const std::exception&) {
            continue;
        }
        ++result.rows_with_valid_timestamp;
        Article article{
            .hash = hash,
            .input_hash = tokenizer.token_input_hash(headline),
            .published_at_utc = fields[published_index],
            .trading_date = date,
            .sector = sector,
            .symbol = symbol,
            .headline = headline,
        };
        result.occurrences.push_back(article);
        result.eligible_dates.insert(date);
        result.eligible_sectors.insert(sector);
        if (seen_hashes.insert(hash).second) ++result.unique_content_hashes;
        if (seen_inputs.insert(article.input_hash).second) {
            ++result.unique_model_inputs;
            if (belongs_to_shard(article.input_hash, shard) &&
                (limit == 0 || result.selected.size() < limit)) {
                result.selected.push_back(std::move(article));
            }
        }
    }
    return result;
}

[[nodiscard]] std::unordered_map<std::string, std::size_t> load_completed_hashes(
    const std::filesystem::path& index_path,
    std::size_t& completed_rows
) {
    std::unordered_map<std::string, std::size_t> completed;
    completed_rows = 0;
    if (!std::filesystem::exists(index_path)) return completed;
    std::ifstream input{index_path};
    if (!input) throw std::runtime_error{"Could not read sector embedding index"};
    std::string line;
    if (!std::getline(input, line)) return completed;
    if (line != "row,input_hash") {
        throw std::runtime_error{"Sector embedding index uses an incompatible input-hash schema"};
    }
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::istringstream row{line};
        std::string index;
        std::string hash;
        if (!std::getline(row, index, ',') || !std::getline(row, hash, ',')) {
            throw std::runtime_error{"Malformed sector embedding index row"};
        }
        const auto row_number = static_cast<std::size_t>(std::stoull(index));
        if (!completed.emplace(hash, row_number).second) {
            throw std::runtime_error{"Duplicate sector embedding hash"};
        }
        ++completed_rows;
    }
    return completed;
}

void write_occurrences(
    const std::filesystem::path& path,
    const std::vector<Article>& occurrences
) {
    std::ofstream output{path};
    if (!output) throw std::runtime_error{"Could not write sector article occurrences"};
    output << "content_hash,input_hash,published_at_utc,trading_date,sector,symbol\n";
    for (const auto& article : occurrences) {
        output << article.hash << ',' << article.input_hash << ',' << article.published_at_utc << ','
               << article.trading_date << ',' << article.sector << ',' << article.symbol << '\n';
    }
}

void write_manifest(
    const std::filesystem::path& path,
    const std::filesystem::path& input_path,
    const std::filesystem::path& model_path,
    const std::filesystem::path& vocab_path,
    const std::string_view from_date,
    const std::string_view to_date,
    const ScanResult& scan,
    const std::size_t batch_size,
    const ShardConfig shard,
    const std::size_t completed_rows,
    const std::size_t failures,
    const double elapsed_seconds
) {
    std::ofstream output{path};
    if (!output) throw std::runtime_error{"Could not write sector embedding manifest"};
    output << std::setprecision(12)
           << "{\n"
           << "  \"input_path\": \"" << input_path.string() << "\",\n"
           << "  \"from_date\": \"" << from_date << "\",\n"
           << "  \"to_date\": \"" << to_date << "\",\n"
           << "  \"input_policy_version\": \"" << kInputPolicyVersion << "\",\n"
           << "  \"model_path\": \"" << model_path.string() << "\",\n"
           << "  \"model_sha256\": \"" << kModelSha256 << "\",\n"
           << "  \"tokenizer_path\": \"" << vocab_path.string() << "\",\n"
           << "  \"tokenizer_sha256\": \"" << kVocabSha256 << "\",\n"
           << "  \"max_tokens\": 64,\n"
           << "  \"sentiment_dimensions\": 4,\n"
           << "  \"sentiment_layout\": \"positive_probability,negative_probability,neutral_probability,sentiment_score\",\n"
           << "  \"batch_size\": " << batch_size << ",\n"
           << "  \"shard_index\": " << shard.index << ",\n"
           << "  \"shard_count\": " << shard.count << ",\n"
           << "  \"rows_read\": " << scan.rows_read << ",\n"
           << "  \"rows_with_valid_timestamp\": " << scan.rows_with_valid_timestamp << ",\n"
           << "  \"occurrence_rows\": " << scan.occurrences.size() << ",\n"
           << "  \"unique_content_hashes\": " << scan.unique_content_hashes << ",\n"
           << "  \"unique_model_inputs\": " << scan.unique_model_inputs << ",\n"
           << "  \"eligible_dates\": " << scan.eligible_dates.size() << ",\n"
           << "  \"eligible_sectors\": " << scan.eligible_sectors.size() << ",\n"
           << "  \"completed_rows\": " << completed_rows << ",\n"
           << "  \"failures\": " << failures << ",\n"
           << "  \"elapsed_seconds\": " << elapsed_seconds << "\n"
           << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3 || argc > 7) {
            std::cout << "Usage: arrakis-build-sector-finbert-embedding-cache <sector-news.csv> "
                         "<cache-dir> [limit] [from-date] [to-date]\n";
            return 0;
        }
        const auto input_path = std::filesystem::path{argv[1]};
        const auto cache_dir = std::filesystem::path{argv[2]};
        const auto limit = argc >= 4 ? static_cast<std::size_t>(std::stoull(argv[3])) : 1000U;
        const auto from_date = argc >= 5 ? std::string{argv[4]} : std::string{"2019-01-01"};
        const auto to_date = argc >= 6 ? std::string{argv[5]} : std::string{"2023-12-31"};
        const auto batch_size = configured_batch_size();
        const auto shard = configured_shard();
        if (from_date > to_date) throw std::invalid_argument{"Invalid date range"};
        std::filesystem::create_directories(cache_dir);

        const auto model_path = std::getenv("ARRAKIS_FINBERT_ONNX_PATH") == nullptr
                                    ? std::filesystem::path{"models/finbert/model_with_pooled_embedding.onnx"}
                                    : std::filesystem::path{std::getenv("ARRAKIS_FINBERT_ONNX_PATH")};
        const auto vocab_path = std::getenv("ARRAKIS_FINBERT_VOCAB_PATH") == nullptr
                                    ? std::filesystem::path{"models/finbert/vocab.txt"}
                                    : std::filesystem::path{std::getenv("ARRAKIS_FINBERT_VOCAB_PATH")};
        arrakis::news::FinbertSession session{
            model_path, vocab_path, "finbert-v1", "finbert-tokenizer-v1", 64
        };
        const auto scan = scan_articles(input_path, from_date, to_date, limit, shard, session);
        const auto index_path = cache_dir / "index.csv";
        const auto embedding_path = cache_dir / "embeddings.f32";
        const auto sentiment_path = cache_dir / "sentiment.f32";
        const auto occurrence_path = cache_dir / "article_occurrences.csv";
        const auto manifest_path = cache_dir / "manifest.json";

        std::size_t completed_rows = 0;
        const auto completed = load_completed_hashes(index_path, completed_rows);
        const auto expected_bytes = completed_rows * kEmbeddingDimensions * sizeof(float);
        const auto expected_sentiment_bytes = completed_rows * kSentimentDimensions * sizeof(float);
        if (std::filesystem::exists(embedding_path)) {
            const auto actual_bytes = std::filesystem::file_size(embedding_path);
            if (actual_bytes < expected_bytes) throw std::runtime_error{"Sector embedding cache is truncated"};
            if (actual_bytes > expected_bytes) std::filesystem::resize_file(embedding_path, expected_bytes);
        } else if (completed_rows != 0) {
            throw std::runtime_error{"Sector embedding index exists without embedding matrix"};
        }
        if (std::filesystem::exists(sentiment_path)) {
            const auto actual_bytes = std::filesystem::file_size(sentiment_path);
            if (actual_bytes < expected_sentiment_bytes) throw std::runtime_error{"Sector sentiment cache is truncated"};
            if (actual_bytes > expected_sentiment_bytes) std::filesystem::resize_file(sentiment_path, expected_sentiment_bytes);
        } else if (completed_rows != 0) {
            throw std::runtime_error{"Sector embedding index exists without sentiment matrix"};
        }
        write_occurrences(occurrence_path, scan.occurrences);
        if (!std::filesystem::exists(index_path)) {
            std::ofstream header{index_path};
            header << "row,input_hash\n";
        }

        std::ofstream embeddings{embedding_path, std::ios::binary | std::ios::app};
        std::ofstream sentiment{sentiment_path, std::ios::binary | std::ios::app};
        std::ofstream index{index_path, std::ios::app};
        if (!embeddings || !sentiment || !index) throw std::runtime_error{"Could not open sector embedding cache outputs"};

        std::vector<Article> pending;
        for (const auto& article : scan.selected) {
            if (!completed.contains(article.input_hash)) pending.push_back(article);
        }
        const auto start = std::chrono::steady_clock::now();
        std::size_t failures = 0;
        for (std::size_t begin = 0; begin < pending.size(); begin += batch_size) {
            const auto end = std::min(pending.size(), begin + batch_size);
            std::vector<std::string> headlines;
            headlines.reserve(end - begin);
            for (std::size_t row = begin; row < end; ++row) headlines.push_back(pending[row].headline);
            const auto outputs = session.infer(headlines);
            if (outputs.size() != headlines.size()) throw std::runtime_error{"FinBERT batch mismatch"};
            for (std::size_t row = 0; row < outputs.size(); ++row) {
                if (outputs[row].pooled_embedding.size() != kEmbeddingDimensions) {
                    ++failures;
                    continue;
                }
                std::vector<float> embedding;
                embedding.reserve(kEmbeddingDimensions);
                for (const auto value : outputs[row].pooled_embedding) embedding.push_back(static_cast<float>(value));
                embeddings.write(
                    reinterpret_cast<const char*>(embedding.data()),
                    static_cast<std::streamsize>(embedding.size() * sizeof(float))
                );
                const std::array<float, kSentimentDimensions> sentiment_values{
                    static_cast<float>(outputs[row].positive_probability),
                    static_cast<float>(outputs[row].negative_probability),
                    static_cast<float>(outputs[row].neutral_probability),
                    static_cast<float>(outputs[row].sentiment_score),
                };
                sentiment.write(
                    reinterpret_cast<const char*>(sentiment_values.data()),
                    static_cast<std::streamsize>(sentiment_values.size() * sizeof(float))
                );
                const auto& article = pending[begin + row];
                index << completed_rows << ',' << article.input_hash << '\n';
                ++completed_rows;
            }
            embeddings.flush();
            sentiment.flush();
            index.flush();
            const auto elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start
            ).count();
            write_manifest(
                manifest_path, input_path, model_path, vocab_path, from_date, to_date,
                scan, batch_size, shard, completed_rows, failures, elapsed
            );
            std::cout << "completed=" << completed_rows << '/' << scan.selected.size()
                      << " elapsed_seconds=" << elapsed << '\n';
        }
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start
        ).count();
        write_manifest(
            manifest_path, input_path, model_path, vocab_path, from_date, to_date, scan,
            batch_size, shard, completed_rows, failures, elapsed
        );
        std::cout << "unique_content_hashes=" << scan.unique_content_hashes
                  << " unique_model_inputs=" << scan.unique_model_inputs
                  << " occurrence_rows=" << scan.occurrences.size()
                  << " eligible_dates=" << scan.eligible_dates.size()
                  << " eligible_sectors=" << scan.eligible_sectors.size()
                  << " completed=" << completed_rows
                  << " elapsed_seconds=" << elapsed << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-build-sector-finbert-embedding-cache: " << error.what() << '\n';
        return 1;
    }
}
