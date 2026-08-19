#include "arrakis/news/aggregation.hpp"
#include "arrakis/news/finbert.hpp"
#include "arrakis/news/market_features.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct Bar final {
    double close{};
    double volume{};
};

struct ArticleRow final {
    std::string date;
    std::string published_at;
    std::string text;
    std::string content_hash;
};

constexpr std::size_t kPooledEmbeddingDimensions = 768;

struct CachedFinbertOutput final {
    float positive_probability{};
    float neutral_probability{};
    float negative_probability{};
    float sentiment_score{};
    std::array<float, kPooledEmbeddingDimensions> pooled_embedding{};
};

[[nodiscard]] CachedFinbertOutput cache_value(
    const arrakis::news::FinbertOutput& output
) {
    if (output.pooled_embedding.size() != kPooledEmbeddingDimensions) {
        throw std::runtime_error{"FinBERT pooled embedding has the wrong dimension"};
    }
    CachedFinbertOutput value{
        .positive_probability = static_cast<float>(output.positive_probability),
        .neutral_probability = static_cast<float>(output.neutral_probability),
        .negative_probability = static_cast<float>(output.negative_probability),
        .sentiment_score = static_cast<float>(output.sentiment_score),
    };
    for (std::size_t index = 0; index < kPooledEmbeddingDimensions; ++index) {
        value.pooled_embedding[index] = static_cast<float>(output.pooled_embedding[index]);
    }
    return value;
}

[[nodiscard]] arrakis::news::FinbertOutput finbert_output(
    const CachedFinbertOutput& value
) {
    arrakis::news::FinbertOutput output;
    output.positive_probability = value.positive_probability;
    output.neutral_probability = value.neutral_probability;
    output.negative_probability = value.negative_probability;
    output.sentiment_score = value.sentiment_score;
    output.pooled_embedding.reserve(kPooledEmbeddingDimensions);
    for (const auto item : value.pooled_embedding) output.pooled_embedding.push_back(item);
    return output;
}

class FinbertCache final {
  public:
    explicit FinbertCache(const std::filesystem::path& directory) : directory_{directory} {
        if (directory_.empty()) return;
        std::filesystem::create_directories(directory_);
        const auto index_path = directory_ / "index.csv";
        const auto values_path = directory_ / "outputs.f32";
        if (std::filesystem::exists(index_path)) {
            std::ifstream index{index_path};
            if (!index) throw std::runtime_error{"Could not read FinBERT cache index"};
            std::string line;
            if (!std::getline(index, line) || line != "row,input_hash") {
                throw std::runtime_error{"FinBERT cache index schema is incompatible"};
            }
            while (std::getline(index, line)) {
                if (line.empty()) continue;
                const auto separator = line.find(',');
                if (separator == std::string::npos || separator == 0 || separator + 1 >= line.size()) {
                    throw std::runtime_error{"Malformed FinBERT cache index row"};
                }
                const auto row = static_cast<std::size_t>(std::stoull(line.substr(0, separator)));
                const auto hash = line.substr(separator + 1);
                if (row != values_.size() || !rows_.emplace(hash, row).second) {
                    throw std::runtime_error{"FinBERT cache index has duplicate or non-contiguous rows"};
                }
                values_.emplace_back();
            }
            const auto expected_bytes = values_.size() * sizeof(CachedFinbertOutput);
            if (!std::filesystem::exists(values_path) ||
                std::filesystem::file_size(values_path) < expected_bytes) {
                throw std::runtime_error{"FinBERT cache output matrix is missing or truncated"};
            }
            // A forced stop can leave a complete binary record after the last
            // flushed index row. Trim that orphan tail and resume safely.
            if (std::filesystem::file_size(values_path) > expected_bytes) {
                std::filesystem::resize_file(values_path, expected_bytes);
            }
            std::ifstream binary{values_path, std::ios::binary};
            if (!binary || (expected_bytes != 0 && !binary.read(
                reinterpret_cast<char*>(values_.data()), static_cast<std::streamsize>(expected_bytes)))) {
                throw std::runtime_error{"Could not read FinBERT cache output matrix"};
            }
        } else if (std::filesystem::exists(values_path)) {
            throw std::runtime_error{"FinBERT cache output matrix exists without an index"};
        }
        values_output_.open(values_path, std::ios::binary | std::ios::app);
        index_output_.open(index_path, std::ios::app);
        if (!values_output_ || !index_output_) throw std::runtime_error{"Could not open FinBERT cache outputs"};
        if (!std::filesystem::exists(index_path) || std::filesystem::file_size(index_path) == 0) {
            index_output_ << "row,input_hash\n";
            index_output_.flush();
        }
    }

    FinbertCache(const FinbertCache&) = delete;
    FinbertCache& operator=(const FinbertCache&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return !directory_.empty(); }

    [[nodiscard]] std::optional<std::size_t> find(const std::string& input_hash) const {
        const auto found = rows_.find(input_hash);
        return found == rows_.end() ? std::nullopt : std::optional<std::size_t>{found->second};
    }

    [[nodiscard]] const CachedFinbertOutput& at(const std::size_t row) const {
        return values_.at(row);
    }

    std::size_t append(const std::string& input_hash, const CachedFinbertOutput& value) {
        if (!enabled()) throw std::logic_error{"Cannot append to a disabled FinBERT cache"};
        const auto row = values_.size();
        if (!rows_.emplace(input_hash, row).second) throw std::runtime_error{"Duplicate FinBERT cache input"};
        values_.push_back(value);
        values_output_.write(
            reinterpret_cast<const char*>(&values_.back()), static_cast<std::streamsize>(sizeof(value))
        );
        index_output_ << row << ',' << input_hash << '\n';
        values_output_.flush();
        index_output_.flush();
        if (!values_output_ || !index_output_) throw std::runtime_error{"Could not append FinBERT cache"};
        return row;
    }

  private:
    std::filesystem::path directory_;
    std::unordered_map<std::string, std::size_t> rows_;
    std::vector<CachedFinbertOutput> values_;
    std::ofstream values_output_;
    std::ofstream index_output_;
};

[[nodiscard]] std::size_t configured_inference_batch_size() {
    constexpr std::size_t default_batch_size = 64;
    const auto* value = std::getenv("ARRAKIS_FINBERT_INFERENCE_BATCH_SIZE");
    if (value == nullptr || *value == '\0') return default_batch_size;
    const auto parsed = static_cast<std::size_t>(std::stoull(value));
    if (parsed == 0 || parsed > 1024) {
        throw std::invalid_argument{
            "ARRAKIS_FINBERT_INFERENCE_BATCH_SIZE must be in [1,1024]"
        };
    }
    return parsed;
}

[[nodiscard]] std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
                field.push_back('"');
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (line[i] == ',' && !quoted) {
            fields.push_back(field);
            field.clear();
        } else {
            field.push_back(line[i]);
        }
    }
    if (quoted) throw std::runtime_error{"Unterminated CSV quote in normalized news"};
    fields.push_back(field);
    return fields;
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
    if (quoted) throw std::runtime_error{"Normalized news ended inside a quoted field"};
    if (!field.empty() || !fields.empty()) fields.push_back(field);
    return fields;
}

[[nodiscard]] std::int64_t parse_utc_ms(const std::string& value) {
    if (value.size() < 19) throw std::invalid_argument{"Invalid UTC timestamp: " + value};
    std::tm parsed{};
    std::istringstream input{value.substr(0, 19)};
    input >> std::get_time(&parsed, "%Y-%m-%d %H:%M:%S");
    if (input.fail()) throw std::invalid_argument{"Invalid UTC timestamp: " + value};
    return static_cast<std::int64_t>(timegm(&parsed)) * 1000;
}

[[nodiscard]] std::int64_t preopen_cutoff_ms(const std::string& date) {
    // 09:20 ET is 13:20 UTC during daylight time and 14:20 UTC otherwise.
    const auto year = std::stoi(date.substr(0, 4));
    const auto month = std::stoi(date.substr(5, 2));
    const auto day = std::stoi(date.substr(8, 2));
    std::tm march{};
    march.tm_year = year - 1900;
    march.tm_mon = 2;
    march.tm_mday = 1;
    const auto march_epoch = timegm(&march);
    const auto march_weekday = gmtime(&march_epoch)->tm_wday;
    const auto second_sunday = 1 + ((7 - march_weekday) % 7) + 7;
    std::tm november{};
    november.tm_year = year - 1900;
    november.tm_mon = 10;
    november.tm_mday = 1;
    const auto november_epoch = timegm(&november);
    const auto november_weekday = gmtime(&november_epoch)->tm_wday;
    const auto first_sunday = 1 + ((7 - november_weekday) % 7);
    const bool daylight = (month > 3 && month < 11) ||
                          (month == 3 && day >= second_sunday) ||
                          (month == 11 && day < first_sunday);
    std::tm cutoff{};
    cutoff.tm_year = year - 1900;
    cutoff.tm_mon = month - 1;
    cutoff.tm_mday = day;
    cutoff.tm_hour = daylight ? 13 : 14;
    cutoff.tm_min = 20;
    return static_cast<std::int64_t>(timegm(&cutoff)) * 1000;
}

[[nodiscard]] std::map<std::string, std::map<std::string, Bar>> load_market(
    const std::filesystem::path& history_dir
) {
    std::map<std::string, std::map<std::string, Bar>> result;
    for (const auto& symbol : {"XLK", "SPY", "QQQ", "IWM", "TLT", "HYG", "GLD", "USO"}) {
        std::ifstream input{history_dir / (std::string{symbol} + ".csv")};
        if (!input) throw std::runtime_error{"Missing market history for " + std::string{symbol}};
        std::string line;
        std::getline(input, line);
        while (std::getline(input, line)) {
            const auto fields = split_csv(line);
            if (fields.size() < 7) continue;
            const auto timestamp = static_cast<std::int64_t>(std::stoll(fields[1]));
            std::tm utc{};
            const auto seconds = static_cast<time_t>(timestamp);
            gmtime_r(&seconds, &utc);
            char date[11]{};
            std::strftime(date, sizeof(date), "%Y-%m-%d", &utc);
            result[symbol][date] = Bar{std::stod(fields[5]), std::stod(fields[6])};
        }
    }
    return result;
}

[[nodiscard]] std::vector<arrakis::news::MarketDay> market_days(
    const std::map<std::string, Bar>& series) {
    std::vector<arrakis::news::MarketDay> output;
    output.reserve(series.size());
    for (const auto& [date, bar] : series) output.push_back({date, bar.close, bar.volume});
    return output;
}

[[nodiscard]] std::vector<ArticleRow> load_news(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open normalized news: " + path.string()};
    const auto header = read_record(input);
    if (header.empty()) throw std::runtime_error{"Normalized news is empty"};
    const auto index = [&](const std::string& name) -> std::size_t {
        const auto found = std::ranges::find(header, name);
        if (found == header.end()) throw std::runtime_error{"News is missing column: " + name};
        return static_cast<std::size_t>(std::distance(header.begin(), found));
    };
    const auto date_i = index("trading_date");
    const auto published_i = index("published_at_utc");
    const auto title_i = index("title");
    const auto summary_i = index("summary");
    const auto content_hash_i = index("content_hash");
    std::vector<ArticleRow> rows;
    for (auto fields = read_record(input); !fields.empty(); fields = read_record(input)) {
        if (fields.size() <= std::max({date_i, published_i, title_i, summary_i, content_hash_i})) continue;
        rows.push_back({
            fields[date_i], fields[published_i], fields[title_i] + " " + fields[summary_i],
            fields[content_hash_i]
        });
    }
    return rows;
}

void write_combined_manifest(
    const std::filesystem::path& output_path,
    const std::filesystem::path& news_path,
    const std::filesystem::path& history_dir,
    const std::filesystem::path& model_path,
    const std::filesystem::path& vocab_path,
    const std::filesystem::path& cache_dir,
    const std::string& from_date,
    const std::string& to_date,
    const bool embedding_output_present
) {
    auto manifest_path = output_path;
    manifest_path += ".manifest.json";
    std::ofstream output{manifest_path};
    if (!output) throw std::runtime_error{"Could not create feature manifest"};
    output << "{\n"
           << "  \"schema\": \""
           << arrakis::news::kCombinedFeatureSchemaHash << "\",\n"
           << "  \"news_schema\": \""
           << arrakis::news::kNewsFeatureSchemaHash << "\",\n"
           << "  \"dataset_path\": \"" << output_path.string() << "\",\n"
           << "  \"normalized_news_path\": \"" << news_path.string() << "\",\n"
           << "  \"market_history_dir\": \"" << history_dir.string() << "\",\n"
           << "  \"finbert_model_path\": \"" << model_path.string() << "\",\n"
           << "  \"tokenizer_path\": \"" << vocab_path.string() << "\",\n"
           << "  \"finbert_cache_dir\": \"" << cache_dir.string() << "\",\n"
           << "  \"finbert_version\": \"finbert-v1\",\n"
           << "  \"tokenizer_version\": \"finbert-tokenizer-v1\",\n"
           << "  \"from_date\": \"" << from_date << "\",\n"
           << "  \"to_date\": \"" << to_date << "\",\n"
           << "  \"embedding_output_required\": false,\n"
           << "  \"embedding_output_present\": "
           << (embedding_output_present ? "true" : "false") << ",\n"
           << "  \"embedding_output_name\": "
           << (embedding_output_present ? "\"pooled_embedding\"" : "null") << ",\n"
           << "  \"embedding_dimension\": 768,\n"
           << "  \"prediction_alignment\": \"market features through close[t]; news eligible through next session 09:20 ET and shifted to prior-session row; target is close[t+1] > close[t]\",\n"
           << "  \"feature_names\": [";
    const auto names = arrakis::news::combined_feature_names();
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (index > 0) output << ", ";
        output << '"' << names[index] << '"';
    }
    output << "]\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 4 || (argc - 4) % 2 != 0) throw std::invalid_argument{
            "Usage: arrakis-build-xlk-combined <normalized-news.csv> <history-dir> "
            "<output.csv> [--from YYYY-MM-DD --to YYYY-MM-DD --cache-dir <dir>]"};
        const auto news_path = std::filesystem::path{argv[1]};
        const auto history_dir = std::filesystem::path{argv[2]};
        const auto output_path = std::filesystem::path{argv[3]};
        std::string from_date{"2019-01-01"};
        std::string to_date{"2023-12-31"};
        std::filesystem::path cache_dir;
        for (int index = 4; index < argc; index += 2) {
            const std::string_view option{argv[index]};
            const std::string value{argv[index + 1]};
            if (option == "--from") {
                from_date = value;
            } else if (option == "--to") {
                to_date = value;
            } else if (option == "--cache-dir") {
                cache_dir = value;
            } else {
                throw std::invalid_argument{"Unknown option: " + std::string{option}};
            }
        }
        if (from_date.empty() || to_date.empty() || from_date > to_date) {
            throw std::invalid_argument{"Builder date range is invalid"};
        }
        const auto model_path = std::getenv("ARRAKIS_FINBERT_ONNX_PATH") == nullptr
                                    ? "models/finbert/model_with_pooled_embedding.onnx"
                                    : std::getenv("ARRAKIS_FINBERT_ONNX_PATH");
        const auto vocab_path = std::getenv("ARRAKIS_FINBERT_VOCAB_PATH") == nullptr
                                    ? "models/finbert/vocab.txt"
                                    : std::getenv("ARRAKIS_FINBERT_VOCAB_PATH");
        arrakis::news::FinbertSession finbert{model_path, vocab_path, "finbert-v1", "finbert-tokenizer-v1"};
        if (!finbert.ready()) throw std::runtime_error{"FinBERT session is not ready"};
        const auto market = load_market(history_dir);
        const auto& xlk_history = market.at("XLK");
        std::vector<std::string> all_market_dates;
        all_market_dates.reserve(xlk_history.size());
        for (const auto& [date, unused] : xlk_history) {
            static_cast<void>(unused);
            all_market_dates.push_back(date);
        }
        std::map<std::string, std::string> prior_session_by_event;
        for (std::size_t index = 1; index < all_market_dates.size(); ++index) {
            prior_session_by_event.emplace(all_market_dates[index], all_market_dates[index - 1]);
        }
        const auto loaded_articles = load_news(news_path);
        std::vector<ArticleRow> articles;
        articles.reserve(loaded_articles.size());
        std::unordered_set<std::string> seen_content_hashes;
        seen_content_hashes.reserve(loaded_articles.size());
        for (const auto& article : loaded_articles) {
            if (article.date < from_date || article.date > to_date) continue;
            // Apply the point-in-time publication cutoff before FinBERT inference.
            // This avoids spending CPU inference time on articles that cannot
            // legally contribute to any prediction-day feature vector.
            const auto published = parse_utc_ms(article.published_at);
            if (!prior_session_by_event.contains(article.date)) continue;
            const auto cutoff = preopen_cutoff_ms(article.date);
            if (published > cutoff) continue;
            const auto dedup_key = article.content_hash.empty()
                                       ? article.date + "|" + article.text
                                       : article.content_hash;
            if (!seen_content_hashes.insert(dedup_key).second) continue;
            articles.push_back(article);
        }
        std::map<std::string, std::vector<arrakis::news::EnrichedArticle>> enriched;
        const auto batch_size = configured_inference_batch_size();
        FinbertCache cache{cache_dir};
        std::vector<std::string> unique_texts;
        std::vector<std::string> unique_hashes;
        std::vector<std::size_t> article_unique_indices;
        article_unique_indices.reserve(articles.size());
        std::unordered_map<std::string, std::size_t> unique_by_hash;
        unique_by_hash.reserve(articles.size());
        std::vector<CachedFinbertOutput> unique_outputs;
        std::vector<std::size_t> pending_unique_indices;
        for (const auto& article : articles) {
            const auto input_hash = finbert.token_input_hash(article.text);
            const auto found = unique_by_hash.find(input_hash);
            if (found != unique_by_hash.end()) {
                article_unique_indices.push_back(found->second);
                continue;
            }
            const auto unique_index = unique_texts.size();
            unique_by_hash.emplace(input_hash, unique_index);
            unique_texts.push_back(article.text);
            unique_hashes.push_back(input_hash);
            article_unique_indices.push_back(unique_index);
            unique_outputs.emplace_back();
            if (const auto cached = cache.find(input_hash); cached.has_value()) {
                unique_outputs.back() = cache.at(*cached);
            } else {
                pending_unique_indices.push_back(unique_index);
            }
        }
        bool embedding_output_present = true;
        for (std::size_t begin = 0; begin < pending_unique_indices.size(); begin += batch_size) {
            const auto end = std::min(pending_unique_indices.size(), begin + batch_size);
            std::vector<std::string> texts;
            texts.reserve(end - begin);
            for (std::size_t i = begin; i < end; ++i) {
                texts.push_back(unique_texts[pending_unique_indices[i]]);
            }
            const auto outputs = finbert.infer(texts);
            if (outputs.size() != end - begin) throw std::runtime_error{"FinBERT batch size mismatch"};
            for (std::size_t i = begin; i < end; ++i) {
                const auto unique_index = pending_unique_indices[i];
                if (outputs[i - begin].pooled_embedding.size() != kPooledEmbeddingDimensions) {
                    embedding_output_present = false;
                }
                unique_outputs[unique_index] = cache_value(outputs[i - begin]);
                if (cache.enabled()) {
                    const auto cache_row = cache.append(unique_hashes[unique_index], unique_outputs[unique_index]);
                    if (cache_row != unique_index && !cache.find(unique_hashes[unique_index]).has_value()) {
                        throw std::runtime_error{"FinBERT cache row assignment changed unexpectedly"};
                    }
                }
            }
            std::cout << "FinBERT cache progress: " << end << '/' << pending_unique_indices.size()
                      << " unique inputs completed\n";
        }
        for (std::size_t index = 0; index < articles.size(); ++index) {
            const auto output = finbert_output(unique_outputs[article_unique_indices[index]]);
            if (output.pooled_embedding.size() != kPooledEmbeddingDimensions) {
                embedding_output_present = false;
            }
            const auto published = parse_utc_ms(articles[index].published_at);
            arrakis::news::EnrichedArticle item;
            item.article.published_at_unix_ms = published;
            item.article.normalized_content_hash = articles[index].content_hash;
            item.article.source_id = "FNSPID";
            item.feature.positive_probability = output.positive_probability;
            item.feature.neutral_probability = output.neutral_probability;
            item.feature.negative_probability = output.negative_probability;
            item.feature.sentiment_score = output.sentiment_score;
            item.feature.pooled_embedding = output.pooled_embedding;
            item.entity_weight = 1.0;
            item.holding_related = true;
            item.sector_related = true;
            const auto prior = prior_session_by_event.find(articles[index].date);
            if (prior == prior_session_by_event.end()) {
                throw std::runtime_error{"News event session has no prior market session"};
            }
            enriched[prior->second].push_back(std::move(item));
        }
        std::filesystem::create_directories(output_path.parent_path());
        std::ofstream output{output_path};
        if (!output) throw std::runtime_error{"Could not create combined dataset"};
        output << "date";
        for (const auto& name : arrakis::news::combined_feature_names()) {
            output << ',' << name;
        }
        output << ",target_next_close_up\n";
        const auto& xlk = xlk_history;
        const auto xlk_days = market_days(xlk);
        const auto spy_days = market_days(market.at("SPY"));
        std::vector<std::string> dates;
        for (const auto& [date, unused] : xlk) {
            static_cast<void>(unused);
            if (date < from_date || date > to_date) continue;
            dates.push_back(date);
        }
        for (std::size_t row = 0; row + 1 < dates.size(); ++row) {
            const auto& date = dates[row];
            const auto& next_date = dates[row + 1];
            const auto market_features = arrakis::news::market_feature_vector(xlk_days, spy_days, date);
            if (!market_features) continue;
            const auto current_close = xlk.at(date).close;
            const auto news_cutoff = preopen_cutoff_ms(next_date);
            const auto daily = enriched.contains(date)
                                   ? arrakis::news::aggregate_daily(date, news_cutoff, std::move(enriched[date]))
                                   : arrakis::news::aggregate_daily(date, news_cutoff, {});
            output << date;
            for (const auto value : *market_features) output << ',' << value;
            for (std::size_t index = 0; index < arrakis::news::kNewsFeatureCount; ++index) {
                output << ',' << daily.values[index];
            }
            output << ',' << (xlk.at(next_date).close > current_close ? 1 : 0) << '\n';
        }
        write_combined_manifest(
            output_path, news_path, history_dir, model_path, vocab_path, cache_dir, from_date, to_date,
            embedding_output_present
        );
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Combined dataset build failed: " << error.what() << '\n';
        return 1;
    }
}
