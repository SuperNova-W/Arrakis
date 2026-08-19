#include "arrakis/news/finbert.hpp"
#include "arrakis/news/xlk_membership.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::size_t kEmbeddingDimensions = 768;
constexpr std::size_t kBatchSize = 64;
constexpr std::string_view kInputPolicyVersion{"headline_v1_max64_after_close_pti_v1"};
constexpr std::string_view kModelSha256{
    "c7f8304257b2a587d9d9b348410b3809cc9403da909cc0331a63294426e4205a"};
constexpr std::string_view kVocabSha256{
    "07eced375cec144d27c900241f3e339dec958f92fddbc551f295c992038a3"};

struct Article final {
    std::string hash;
    std::string date;
    std::string symbol;
    std::string headline;
};

struct ScanResult final {
    std::vector<Article> selected;
    std::size_t rows_read{};
    std::size_t rows_with_valid_timestamp{};
    std::size_t rows_with_membership{};
    std::size_t unique_eligible{};
    std::set<std::string> eligible_dates;
    std::set<std::string> eligible_sectors;
};

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
    if (quoted) throw std::runtime_error{"CSV ended inside a quoted field"};
    if (!field.empty() || !fields.empty()) fields.push_back(field);
    return fields;
}

[[nodiscard]] std::int64_t parse_utc_ms(const std::string_view value) {
    if (value.size() < 19) throw std::invalid_argument{"Invalid UTC timestamp"};
    std::tm parsed{};
    std::istringstream input{std::string{value.substr(0, 19)}};
    input >> std::get_time(&parsed, "%Y-%m-%d %H:%M:%S");
    if (input.fail()) throw std::invalid_argument{"Invalid UTC timestamp"};
    return static_cast<std::int64_t>(timegm(&parsed)) * 1000;
}

[[nodiscard]] std::size_t column_index(
    const std::vector<std::string>& header,
    const std::string_view name
) {
    const auto found = std::ranges::find(header, name);
    if (found == header.end()) throw std::runtime_error{"Missing CSV column: " + std::string{name}};
    return static_cast<std::size_t>(std::distance(header.begin(), found));
}

[[nodiscard]] ScanResult scan_articles(
    const std::filesystem::path& input_path,
    const arrakis::news::XlkMembershipResolver& membership,
    const std::string_view from_date,
    const std::string_view to_date,
    const std::size_t limit
) {
    std::ifstream input{input_path};
    if (!input) throw std::runtime_error{"Could not open normalized news: " + input_path.string()};
    const auto header = read_record(input);
    const auto date_index = column_index(header, "trading_date");
    const auto published_index = column_index(header, "published_at_utc");
    const auto symbol_index = column_index(header, "symbol");
    const auto title_index = column_index(header, "title");
    const auto hash_index = column_index(header, "content_hash");

    ScanResult result;
    std::unordered_set<std::string> seen_hashes;
    while (true) {
        const auto fields = read_record(input);
        if (fields.empty()) break;
        ++result.rows_read;
        const auto max_index = std::max(
            {date_index, published_index, symbol_index, title_index, hash_index}
        );
        if (fields.size() <= max_index) continue;
        const auto& date = fields[date_index];
        const auto& symbol = fields[symbol_index];
        const auto& headline = fields[title_index];
        const auto& hash = fields[hash_index];
        if (date < from_date || date > to_date || headline.empty() || hash.empty()) continue;
        try {
            static_cast<void>(parse_utc_ms(fields[published_index]));
        } catch (const std::exception&) {
            continue;
        }
        ++result.rows_with_valid_timestamp;
        if (!membership.held_on(symbol, date)) continue;
        ++result.rows_with_membership;
        if (!seen_hashes.insert(hash).second) continue;
        ++result.unique_eligible;
        result.eligible_dates.insert(date);
        result.eligible_sectors.insert("XLK");
        if (limit == 0 || result.selected.size() < limit) {
            result.selected.push_back(Article{
                .hash = hash, .date = date, .symbol = symbol, .headline = headline
            });
        }
    }
    return result;
}

[[nodiscard]] std::unordered_set<std::string> load_completed_hashes(
    const std::filesystem::path& index_path,
    std::size_t& completed_rows
) {
    std::unordered_set<std::string> completed;
    completed_rows = 0;
    if (!std::filesystem::exists(index_path)) return completed;
    std::ifstream input{index_path};
    if (!input) throw std::runtime_error{"Could not read cache index"};
    std::string line;
    if (!std::getline(input, line)) return completed;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::istringstream row{line};
        std::string index;
        std::string hash;
        if (!std::getline(row, index, ',') || !std::getline(row, hash, ',')) {
            throw std::runtime_error{"Malformed cache index row"};
        }
        if (!completed.insert(hash).second) throw std::runtime_error{"Duplicate cache hash"};
        ++completed_rows;
    }
    return completed;
}

void write_manifest(
    const std::filesystem::path& path,
    const std::filesystem::path& input_path,
    const std::filesystem::path& model_path,
    const std::filesystem::path& vocab_path,
    const std::string_view from_date,
    const std::string_view to_date,
    const ScanResult& scan,
    const std::size_t completed_rows,
    const std::size_t failures,
    const double elapsed_seconds
) {
    std::ofstream output{path};
    if (!output) throw std::runtime_error{"Could not write embedding cache manifest"};
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
           << "  \"batch_size\": " << kBatchSize << ",\n"
           << "  \"rows_read\": " << scan.rows_read << ",\n"
           << "  \"rows_with_valid_timestamp\": " << scan.rows_with_valid_timestamp << ",\n"
           << "  \"rows_with_historical_membership\": " << scan.rows_with_membership << ",\n"
           << "  \"unique_eligible_articles\": " << scan.unique_eligible << ",\n"
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
        if (argc < 4 || argc > 7) {
            std::cout << "Usage: arrakis-build-finbert-embedding-cache <news.csv> <holdings.csv> "
                         "<cache-dir> [limit] [from-date] [to-date]\n";
            return 0;
        }
        const auto news_path = std::filesystem::path{argv[1]};
        const auto holdings_path = std::filesystem::path{argv[2]};
        const auto cache_dir = std::filesystem::path{argv[3]};
        const auto limit = argc >= 5 ? static_cast<std::size_t>(std::stoull(argv[4])) : 1000U;
        const auto from_date = argc >= 6 ? std::string{argv[5]} : std::string{"2019-01-01"};
        const auto to_date = argc >= 7 ? std::string{argv[6]} : std::string{"2023-12-31"};
        if (from_date > to_date) throw std::invalid_argument{"Invalid date range"};
        std::filesystem::create_directories(cache_dir);

        const auto membership = arrakis::news::XlkMembershipResolver::from_csv(holdings_path);
        auto scan = scan_articles(news_path, membership, from_date, to_date, limit);
        const auto model_path = std::getenv("ARRAKIS_FINBERT_ONNX_PATH") == nullptr
                                    ? std::filesystem::path{"models/finbert/model_with_pooled_embedding.onnx"}
                                    : std::filesystem::path{std::getenv("ARRAKIS_FINBERT_ONNX_PATH")};
        const auto vocab_path = std::getenv("ARRAKIS_FINBERT_VOCAB_PATH") == nullptr
                                    ? std::filesystem::path{"models/finbert/vocab.txt"}
                                    : std::filesystem::path{std::getenv("ARRAKIS_FINBERT_VOCAB_PATH")};
        const auto index_path = cache_dir / "index.csv";
        const auto embedding_path = cache_dir / "embeddings.f32";
        const auto manifest_path = cache_dir / "manifest.json";

        std::size_t completed_rows = 0;
        auto completed = load_completed_hashes(index_path, completed_rows);
        const auto expected_bytes = completed_rows * kEmbeddingDimensions * sizeof(float);
        if (std::filesystem::exists(embedding_path)) {
            const auto actual_bytes = std::filesystem::file_size(embedding_path);
            if (actual_bytes < expected_bytes) throw std::runtime_error{"Embedding cache is truncated"};
            if (actual_bytes > expected_bytes) std::filesystem::resize_file(embedding_path, expected_bytes);
        } else if (completed_rows != 0) {
            throw std::runtime_error{"Cache index exists without embedding matrix"};
        }
        if (!std::filesystem::exists(index_path)) {
            std::ofstream header{index_path};
            header << "row,content_hash,trading_date,symbol,positive,negative,neutral\n";
        }

        std::ofstream embeddings{embedding_path, std::ios::binary | std::ios::app};
        std::ofstream index{index_path, std::ios::app};
        if (!embeddings || !index) throw std::runtime_error{"Could not open embedding cache outputs"};

        std::vector<Article> pending;
        for (const auto& article : scan.selected) {
            if (!completed.contains(article.hash)) pending.push_back(article);
        }
        const auto start = std::chrono::steady_clock::now();
        std::size_t failures = 0;
        arrakis::news::FinbertSession session{
            model_path, vocab_path, "finbert-v1", "finbert-tokenizer-v1", 64
        };
        for (std::size_t begin = 0; begin < pending.size(); begin += kBatchSize) {
            const auto end = std::min(pending.size(), begin + kBatchSize);
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
                const auto& article = pending[begin + row];
                index << completed_rows << ',' << article.hash << ',' << article.date << ','
                      << article.symbol << ',' << outputs[row].positive_probability << ','
                      << outputs[row].negative_probability << ',' << outputs[row].neutral_probability << '\n';
                ++completed_rows;
            }
            embeddings.flush();
            index.flush();
            const auto elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start
            ).count();
            write_manifest(
                manifest_path, news_path, model_path, vocab_path, from_date, to_date,
                scan, completed_rows, failures, elapsed
            );
            std::cout << "completed=" << completed_rows << '/' << scan.selected.size()
                      << " elapsed_seconds=" << elapsed << '\n';
        }
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start
        ).count();
        write_manifest(
            manifest_path, news_path, model_path, vocab_path, from_date, to_date, scan,
            completed_rows, failures, elapsed
        );
        std::cout << "eligible_unique=" << scan.unique_eligible
                  << " eligible_dates=" << scan.eligible_dates.size()
                  << " eligible_sectors=" << scan.eligible_sectors.size()
                  << " completed=" << completed_rows
                  << " elapsed_seconds=" << elapsed << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-build-finbert-embedding-cache: " << error.what() << '\n';
        return 1;
    }
}
