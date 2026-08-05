#include "arrakis/news/aggregation.hpp"
#include "arrakis/news/finbert.hpp"
#include "arrakis/news/market_features.hpp"

#include <algorithm>
#include <cmath>
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
};

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

[[nodiscard]] std::int64_t market_close_ms(const std::string& date) {
    // US daylight-saving window: second Sunday in March through first Sunday
    // in November. Close is 20:00 UTC in DST and 21:00 UTC otherwise.
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
    std::tm close{};
    close.tm_year = year - 1900;
    close.tm_mon = month - 1;
    close.tm_mday = day;
    close.tm_hour = daylight ? 20 : 21;
    return static_cast<std::int64_t>(timegm(&close)) * 1000;
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
    std::vector<ArticleRow> rows;
    for (auto fields = read_record(input); !fields.empty(); fields = read_record(input)) {
        if (fields.size() <= std::max({date_i, published_i, title_i, summary_i})) continue;
        rows.push_back({fields[date_i], fields[published_i], fields[title_i] + " " + fields[summary_i]});
    }
    return rows;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::invalid_argument{
            "Usage: arrakis-build-xlk-combined <normalized-news.csv> <history-dir> <output.csv>"};
        const auto news_path = std::filesystem::path{argv[1]};
        const auto history_dir = std::filesystem::path{argv[2]};
        const auto output_path = std::filesystem::path{argv[3]};
        const auto model_path = std::getenv("ARRAKIS_FINBERT_ONNX_PATH") == nullptr
                                    ? "models/finbert/model.onnx"
                                    : std::getenv("ARRAKIS_FINBERT_ONNX_PATH");
        const auto vocab_path = std::getenv("ARRAKIS_FINBERT_VOCAB_PATH") == nullptr
                                    ? "models/finbert/vocab.txt"
                                    : std::getenv("ARRAKIS_FINBERT_VOCAB_PATH");
        arrakis::news::FinbertSession finbert{model_path, vocab_path, "finbert-v1", "finbert-tokenizer-v1"};
        if (!finbert.ready()) throw std::runtime_error{"FinBERT session is not ready"};
        const auto market = load_market(history_dir);
        auto articles = load_news(news_path);
        std::map<std::string, std::vector<arrakis::news::EnrichedArticle>> enriched;
        constexpr std::size_t batch_size = 64;
        for (std::size_t begin = 0; begin < articles.size(); begin += batch_size) {
            const auto end = std::min(articles.size(), begin + batch_size);
            std::vector<std::string> texts;
            texts.reserve(end - begin);
            for (std::size_t i = begin; i < end; ++i) texts.push_back(articles[i].text);
            const auto outputs = finbert.infer(texts);
            if (outputs.size() != end - begin) throw std::runtime_error{"FinBERT batch size mismatch"};
            for (std::size_t i = begin; i < end; ++i) {
                const auto published = parse_utc_ms(articles[i].published_at);
                const auto cutoff = market_close_ms(articles[i].date);
                if (published > cutoff) continue;
                arrakis::news::EnrichedArticle item;
                item.article.published_at_unix_ms = published;
                item.article.normalized_content_hash = "fnspid";
                item.article.source_id = "FNSPID";
                item.feature.positive_probability = outputs[i - begin].positive_probability;
                item.feature.neutral_probability = outputs[i - begin].neutral_probability;
                item.feature.negative_probability = outputs[i - begin].negative_probability;
                item.feature.sentiment_score = outputs[i - begin].sentiment_score;
                item.feature.pooled_embedding = outputs[i - begin].pooled_embedding;
                item.entity_weight = 1.0;
                item.holding_related = true;
                item.sector_related = true;
                enriched[articles[i].date].push_back(std::move(item));
            }
        }
        std::filesystem::create_directories(output_path.parent_path());
        std::ofstream output{output_path};
        if (!output) throw std::runtime_error{"Could not create combined dataset"};
        output << "date";
        for (const auto& name : arrakis::news::combined_feature_names()) output << ',' << name;
        output << ",target_next_close_up\n";
        const auto& xlk = market.at("XLK");
        const auto xlk_days = market_days(xlk);
        const auto spy_days = market_days(market.at("SPY"));
        std::vector<std::string> dates;
        for (const auto& [date, unused] : xlk) {
            static_cast<void>(unused);
            if (date < "2019-01-01" || date > "2023-12-31") continue;
            dates.push_back(date);
        }
        for (std::size_t row = 6; row + 1 < dates.size(); ++row) {
            const auto& date = dates[row];
            const auto& next_date = dates[row + 1];
            const auto market_features = arrakis::news::market_feature_vector(xlk_days, spy_days, date);
            if (!market_features) throw std::runtime_error{"Market history is insufficient for " + date};
            const auto current_close = xlk.at(date).close;
            const auto daily = enriched.contains(date)
                                   ? arrakis::news::aggregate_daily(date, market_close_ms(date), std::move(enriched[date]))
                                   : arrakis::news::aggregate_daily(date, market_close_ms(date), {});
            output << date;
            for (const auto value : *market_features) output << ',' << value;
            for (const auto value : daily.values) output << ',' << value;
            output << ',' << (xlk.at(next_date).close > current_close ? 1 : 0) << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Combined dataset build failed: " << error.what() << '\n';
        return 1;
    }
}
