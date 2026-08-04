#include "arrakis/news/finbert.hpp"

#include <filesystem>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <stdexcept>
#include <utility>

#if defined(ARRAKIS_HAS_ONNXRUNTIME)
#include <onnxruntime_cxx_api.h>
#endif

namespace arrakis::news {

struct FinbertSession::Impl final {
    std::string model_path;
    std::string vocab_path;
    std::string model_version;
    std::string tokenizer_version;
    std::size_t max_tokens{};
    bool ready{};
#if defined(ARRAKIS_HAS_ONNXRUNTIME)
    Impl(std::string model, std::string vocab, std::string model_id, std::string tokenizer_id, std::size_t tokens)
        : model_path(std::move(model)), vocab_path(std::move(vocab)), model_version(std::move(model_id)), tokenizer_version(std::move(tokenizer_id)), max_tokens(tokens) {}
    Ort::Env environment{ORT_LOGGING_LEVEL_WARNING, "arrakis-finbert"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    std::unordered_map<std::string, std::int64_t> vocabulary;
#else
    Impl(std::string model, std::string vocab, std::string model_id, std::string tokenizer_id, std::size_t tokens)
        : model_path(std::move(model)), vocab_path(std::move(vocab)), model_version(std::move(model_id)), tokenizer_version(std::move(tokenizer_id)), max_tokens(tokens) {}
#endif
};

#if defined(ARRAKIS_HAS_ONNXRUNTIME)
std::vector<std::string> wordpiece_tokens(std::string text, const std::unordered_map<std::string, std::int64_t>& vocabulary) {
    std::vector<std::string> basic;
    std::string token;
    const auto flush = [&]() { if (!token.empty()) { basic.push_back(token); token.clear(); } };
    for (const auto character : text) {
        const auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        if (std::isspace(static_cast<unsigned char>(lower))) { flush(); continue; }
        if (std::ispunct(static_cast<unsigned char>(lower))) { flush(); basic.emplace_back(1, lower); continue; }
        token.push_back(lower);
    }
    flush();
    std::vector<std::string> output;
    for (const auto& word : basic) {
        if (vocabulary.contains(word)) { output.push_back(word); continue; }
        std::size_t start = 0; bool failed = false;
        while (start < word.size()) {
            std::size_t end = word.size(); std::string match;
            while (start < end) {
                std::string candidate = word.substr(start, end - start);
                if (start > 0) candidate = "##" + candidate;
                if (vocabulary.contains(candidate)) { match = std::move(candidate); break; }
                --end;
            }
            if (match.empty()) { failed = true; break; }
            output.push_back(std::move(match)); start = end;
        }
        if (failed) output.emplace_back("[UNK]");
    }
    return output;
}
#endif

FinbertSession::FinbertSession(std::string model_path, std::string vocab_path, std::string model_version,
                               std::string tokenizer_version, std::size_t max_tokens)
    : impl_(std::make_unique<Impl>(std::move(model_path), std::move(vocab_path), std::move(model_version), std::move(tokenizer_version), max_tokens)) {
    if (impl_->model_path.empty() || impl_->vocab_path.empty()) throw std::invalid_argument("FinBERT model and vocabulary paths are required");
    if (!std::filesystem::exists(impl_->model_path) || !std::filesystem::exists(impl_->vocab_path)) throw std::runtime_error("FinBERT ONNX artifact or tokenizer vocabulary is missing");
#if defined(ARRAKIS_HAS_ONNXRUNTIME)
    std::ifstream vocabulary_file(impl_->vocab_path);
    std::string token; std::int64_t id = 0;
    while (std::getline(vocabulary_file, token)) { if (!token.empty()) impl_->vocabulary.emplace(token, id); ++id; }
    if (!impl_->vocabulary.contains("[CLS]") || !impl_->vocabulary.contains("[SEP]") || !impl_->vocabulary.contains("[PAD]")) throw std::runtime_error("FinBERT vocabulary is missing required special tokens");
    impl_->options.SetIntraOpNumThreads(1); impl_->options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
    impl_->session = std::make_unique<Ort::Session>(impl_->environment, impl_->model_path.c_str(), impl_->options);
    impl_->ready = true;
#else
    throw std::runtime_error("FinBERT ONNX Runtime support is not enabled in this build; install the pinned ONNX Runtime C++ package");
#endif
}

FinbertSession::~FinbertSession() = default;
bool FinbertSession::ready() const noexcept { return impl_ != nullptr && impl_->ready; }
const std::string& FinbertSession::model_version() const noexcept { return impl_->model_version; }
const std::string& FinbertSession::tokenizer_version() const noexcept { return impl_->tokenizer_version; }

std::vector<FinbertOutput> FinbertSession::infer(const std::vector<std::string>& texts) const {
    if (!ready()) throw std::runtime_error("FinBERT session is not ready");
    if (texts.empty()) return {};
#if defined(ARRAKIS_HAS_ONNXRUNTIME)
    Ort::AllocatorWithDefaultOptions allocator;
    const auto input_count = impl_->session->GetInputCount();
    if (input_count < 2) throw std::runtime_error("FinBERT ONNX model must expose input_ids and attention_mask");
    std::vector<std::string> input_names_storage; input_names_storage.reserve(input_count);
    for (std::size_t index = 0; index < input_count; ++index) input_names_storage.emplace_back(impl_->session->GetInputNameAllocated(index, allocator).get());
    std::vector<std::vector<std::int64_t>> ids(texts.size()), masks(texts.size()), types(texts.size());
    const auto pad_id = impl_->vocabulary.at("[PAD]"), cls_id = impl_->vocabulary.at("[CLS]"), sep_id = impl_->vocabulary.at("[SEP]");
    for (std::size_t row = 0; row < texts.size(); ++row) {
        ids[row].push_back(cls_id);
        for (const auto& token : wordpiece_tokens(texts[row], impl_->vocabulary)) { if (ids[row].size() + 1 >= impl_->max_tokens) break; ids[row].push_back(impl_->vocabulary.contains(token) ? impl_->vocabulary.at(token) : impl_->vocabulary.at("[UNK]")); }
        ids[row].push_back(sep_id); ids[row].resize(impl_->max_tokens, pad_id); masks[row].assign(impl_->max_tokens, 0); types[row].assign(impl_->max_tokens, 0); for (std::size_t index = 0; index < impl_->max_tokens && ids[row][index] != pad_id; ++index) masks[row][index] = 1;
    }
    std::vector<std::int64_t> flat_ids, flat_masks, flat_types; for (std::size_t row = 0; row < texts.size(); ++row) { flat_ids.insert(flat_ids.end(), ids[row].begin(), ids[row].end()); flat_masks.insert(flat_masks.end(), masks[row].begin(), masks[row].end()); flat_types.insert(flat_types.end(), types[row].begin(), types[row].end()); }
    const std::array<std::int64_t, 2> shape{static_cast<std::int64_t>(texts.size()), static_cast<std::int64_t>(impl_->max_tokens)};
    Ort::MemoryInfo memory_info("Cpu", OrtDeviceAllocator, 0, OrtMemTypeDefault);
    std::vector<Ort::Value> inputs;
    inputs.push_back(Ort::Value::CreateTensor<std::int64_t>(memory_info, flat_ids.data(), flat_ids.size(), shape.data(), shape.size()));
    inputs.push_back(Ort::Value::CreateTensor<std::int64_t>(memory_info, flat_masks.data(), flat_masks.size(), shape.data(), shape.size()));
    if (input_count > 2) inputs.push_back(Ort::Value::CreateTensor<std::int64_t>(memory_info, flat_types.data(), flat_types.size(), shape.data(), shape.size()));
    std::vector<const char*> input_names; for (const auto& name : input_names_storage) input_names.push_back(name.c_str());
    const auto output_count = impl_->session->GetOutputCount(); std::vector<std::string> output_names_storage; std::vector<const char*> output_names; for (std::size_t index = 0; index < output_count; ++index) { output_names_storage.emplace_back(impl_->session->GetOutputNameAllocated(index, allocator).get()); output_names.push_back(output_names_storage.back().c_str()); }
    const auto outputs = impl_->session->Run(Ort::RunOptions{nullptr}, input_names.data(), inputs.data(), inputs.size(), output_names.data(), output_names.size());
    if (outputs.empty()) throw std::runtime_error("FinBERT ONNX model returned no outputs");
    const auto* logits = outputs[0].GetTensorData<float>(); const auto logits_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape(); if (logits == nullptr || logits_shape.size() != 2 || logits_shape[0] != static_cast<std::int64_t>(texts.size()) || logits_shape[1] < 3) throw std::runtime_error("FinBERT logits output has an invalid shape");
    std::vector<FinbertOutput> result(texts.size()); for (std::size_t row = 0; row < texts.size(); ++row) { const double max_logit = std::max({static_cast<double>(logits[row * 3]), static_cast<double>(logits[row * 3 + 1]), static_cast<double>(logits[row * 3 + 2])}); double sum = 0; double probabilities[3]{}; for (std::size_t index = 0; index < 3; ++index) { probabilities[index] = std::exp(static_cast<double>(logits[row * 3 + index]) - max_logit); sum += probabilities[index]; } result[row].positive_probability = probabilities[0] / sum; result[row].negative_probability = probabilities[1] / sum; result[row].neutral_probability = probabilities[2] / sum; result[row].sentiment_score = result[row].positive_probability - result[row].negative_probability; }
    if (output_count > 1) { const auto* embeddings = outputs[1].GetTensorData<float>(); const auto embedding_shape = outputs[1].GetTensorTypeAndShapeInfo().GetShape(); if (embeddings != nullptr && embedding_shape.size() == 2 && embedding_shape[0] == static_cast<std::int64_t>(texts.size())) for (std::size_t row = 0; row < texts.size(); ++row) { const auto width = static_cast<std::size_t>(embedding_shape[1]); result[row].pooled_embedding.assign(embeddings + row * width, embeddings + (row + 1) * width); } }
    return result;
#else
    static_cast<void>(texts);
    throw std::runtime_error("FinBERT ONNX Runtime support is unavailable");
#endif
}

}  // namespace arrakis::news
