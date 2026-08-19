#include "arrakis/news/finbert.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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
    if (found == header.end()) throw std::runtime_error{"Missing SEC event column: " + std::string{name}};
    return static_cast<std::size_t>(std::distance(header.begin(), found));
}

[[nodiscard]] std::string env(const char* name, std::string fallback) {
    const auto* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? std::move(fallback) : std::string{value};
}

[[nodiscard]] std::string strip_html(std::string_view html) {
    std::string text;
    text.reserve(std::min<std::size_t>(html.size(), 8192));
    std::size_t skipped_depth = 0;
    for (std::size_t index = 0; index < html.size();) {
        if (html[index] != '<') {
            if (skipped_depth == 0) {
                const auto character = static_cast<unsigned char>(html[index]);
                text.push_back(std::isspace(character) ? ' ' : static_cast<char>(character));
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
        std::ranges::transform(tag, tag.begin(), [](const char character) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        });
        const bool closing = tag.size() > 1 && tag[1] == '/';
        const bool self_closing = tag.size() > 2 && tag[tag.size() - 2] == '/';
        const bool hidden_open = !closing &&
            (tag.starts_with("<ix:header") ||
             (tag.starts_with("<div") && tag.find("display") != std::string::npos &&
              tag.find("none") != std::string::npos));
        const bool script_or_style_open = !closing &&
            (tag.starts_with("<script") || tag.starts_with("<style"));
        if (skipped_depth > 0) {
            if (closing) {
                --skipped_depth;
            } else if (!self_closing && !tag.starts_with("<!")) {
                ++skipped_depth;
            }
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

[[nodiscard]] std::string semantic_filing_text(const std::string& body) {
    std::string lower = body;
    std::ranges::transform(lower, lower.begin(), [](const char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    });
    std::vector<std::size_t> item_starts;
    for (std::size_t position = lower.find("item "); position != std::string::npos;
         position = lower.find("item ", position + 5)) {
        const bool word_boundary = position == 0 || !std::isalnum(static_cast<unsigned char>(lower[position - 1]));
        const auto number = position + 5;
        if (word_boundary && number < lower.size() && std::isdigit(static_cast<unsigned char>(lower[number]))) {
            item_starts.push_back(position);
        }
    }
    if (item_starts.empty()) return body;

    std::string selected;
    selected.reserve(std::min<std::size_t>(body.size(), 8192));
    for (std::size_t index = 0; index < item_starts.size(); ++index) {
        const auto begin = item_starts[index];
        const auto end = index + 1 < item_starts.size() ? item_starts[index + 1] : body.size();
        const auto section = lower.substr(begin, std::min<std::size_t>(end - begin, 16));
        if (section.starts_with("item 9.01")) continue;
        if (!selected.empty()) selected.push_back(' ');
        selected.append(body, begin, end - begin);
        if (selected.size() >= 8192) break;
    }
    return selected.empty() ? body : selected.substr(0, 8192);
}

struct Article final {
    std::string article_id;
    std::string trading_date;
    std::string sector;
    std::string symbol;
    std::string form;
    std::string text;
    std::string input_hash;
};

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 4) {
            std::cout << "Usage: arrakis-build-sec-finbert-sentiment-cache <sec_events.csv> "
                         "<html_dir> <output.csv>\n";
            return 0;
        }
        std::ifstream events{argv[1]};
        if (!events) throw std::runtime_error{"Could not open SEC events"};
        const auto header = read_record(events);
        const auto article_id = column_index(header, "article_id");
        const auto trading_date = column_index(header, "trading_date");
        const auto sector = column_index(header, "sector");
        const auto symbol = column_index(header, "symbol");
        const auto form = column_index(header, "form");
        std::vector<Article> articles;
        std::size_t rows_read = 0;
        std::size_t missing_html = 0;
        while (true) {
            const auto row = read_record(events);
            if (row.empty()) break;
            ++rows_read;
            if (row.size() != header.size()) throw std::runtime_error{"Malformed SEC event row"};
            if (!row[form].starts_with("8-K")) continue;
            const auto html_path = std::filesystem::path{argv[2]} / (row[article_id] + ".html");
            if (!std::filesystem::exists(html_path)) {
                ++missing_html;
                continue;
            }
            std::ifstream input{html_path};
            std::ostringstream content;
            content << input.rdbuf();
            const auto body = semantic_filing_text(strip_html(content.str()));
            if (body.empty()) {
                ++missing_html;
                continue;
            }
            Article article{
                .article_id = row[article_id], .trading_date = row[trading_date], .sector = row[sector],
                .symbol = row[symbol], .form = row[form],
                .text = body,
            };
            articles.push_back(std::move(article));
        }

        const auto model_path = env("ARRAKIS_FINBERT_ONNX_PATH", "models/finbert/model.onnx");
        const auto vocab_path = env("ARRAKIS_FINBERT_VOCAB_PATH", "models/finbert/vocab.txt");
        const auto batch_text = env("ARRAKIS_FINBERT_SENTIMENT_BATCH_SIZE", "128");
        const auto batch_size = static_cast<std::size_t>(std::stoull(batch_text));
        if (batch_size == 0 || batch_size > 1024) throw std::invalid_argument{"Invalid sentiment batch size"};
        arrakis::news::FinbertSession session{model_path, vocab_path, "finbert-v1", "finbert-tokenizer-v1", 64};

        std::unordered_map<std::string, std::size_t> input_index;
        std::vector<std::string> unique_texts;
        for (auto& article : articles) {
            article.input_hash = session.token_input_hash(article.text);
            if (!input_index.contains(article.input_hash)) {
                input_index.emplace(article.input_hash, unique_texts.size());
                unique_texts.push_back(article.text);
            }
        }
        std::vector<arrakis::news::FinbertOutput> outputs(unique_texts.size());
        const auto started = std::chrono::steady_clock::now();
        for (std::size_t begin = 0; begin < unique_texts.size(); begin += batch_size) {
            const auto end = std::min(unique_texts.size(), begin + batch_size);
            std::vector<std::string> batch;
            batch.reserve(end - begin);
            for (std::size_t index = begin; index < end; ++index) batch.push_back(unique_texts[index]);
            const auto inferred = session.infer(batch);
            if (inferred.size() != batch.size()) throw std::runtime_error{"FinBERT sentiment batch mismatch"};
            for (std::size_t index = 0; index < inferred.size(); ++index) outputs[begin + index] = inferred[index];
            std::cout << "completed=" << end << '/' << unique_texts.size() << '\n';
        }

        std::ofstream output{argv[3]};
        if (!output) throw std::runtime_error{"Could not write FinBERT sentiment cache"};
        output << "article_id,trading_date,sector,symbol,form,sentiment_score,positive_probability,negative_probability,neutral_probability,text_chars,input_hash\n";
        for (const auto& article : articles) {
            const auto index = input_index.at(article.input_hash);
            const auto& sentiment = outputs[index];
            output << article.article_id << ',' << article.trading_date << ',' << article.sector << ',' << article.symbol
                   << ',' << article.form << ',' << sentiment.sentiment_score << ',' << sentiment.positive_probability
                   << ',' << sentiment.negative_probability << ',' << sentiment.neutral_probability << ','
                   << article.text.size() << ',' << article.input_hash << '\n';
        }
        std::ofstream manifest{std::string{argv[3]} + ".manifest.json"};
        if (!manifest) throw std::runtime_error{"Could not write FinBERT sentiment manifest"};
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        manifest << std::setprecision(12) << "{\n"
                 << "  \"source\": \"SEC 8-K primary documents\",\n"
                 << "  \"rows_read\": " << rows_read << ",\n"
                 << "  \"rows_with_html\": " << articles.size() << ",\n"
                 << "  \"missing_html\": " << missing_html << ",\n"
                 << "  \"unique_model_inputs\": " << unique_texts.size() << ",\n"
                 << "  \"max_tokens\": 64,\n"
                 << "  \"text_extraction\": \"visible HTML text only; omit ix:header, display:none, script/style, and comments; retain Item sections except Item 9.01; truncate at 8192 chars\",\n"
                 << "  \"model_path\": \"" << model_path << "\",\n"
                 << "  \"tokenizer_path\": \"" << vocab_path << "\",\n"
                 << "  \"elapsed_seconds\": " << elapsed << "\n}\n";
        std::cout << "SEC FinBERT sentiment: " << articles.size() << " rows, " << unique_texts.size()
                  << " unique inputs, elapsed_seconds=" << elapsed << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-build-sec-finbert-sentiment-cache: " << error.what() << '\n';
        return 1;
    }
}
