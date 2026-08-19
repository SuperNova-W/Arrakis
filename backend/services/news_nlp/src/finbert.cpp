#include "arrakis/news/finbert.hpp"

#include <openssl/sha.h>

#include <filesystem>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <stdexcept>
#include <utility>

#if defined(ARRAKIS_HAS_ONNXRUNTIME)
#include <onnxruntime_cxx_api.h>
#if defined(__APPLE__) && __has_include(<coreml_provider_factory.h>)
#include <coreml_provider_factory.h>
#define ARRAKIS_HAS_COREML_PROVIDER 1
#endif
#endif

namespace arrakis::news {

struct FinbertSession::Impl final {
    std::string model_path;
    std::string vocab_path;
    std::string model_version;
    std::string tokenizer_version;
    std::size_t max_tokens{};
    bool ready{};
    bool require_embedding_output{};
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
std::vector<std::string> wordpiece_tokens(
    const std::string_view text,
    const std::unordered_map<std::string, std::int64_t>& vocabulary,
    const std::size_t maximum_tokens
) {
    std::vector<std::string> output;
    output.reserve(maximum_tokens);
    std::string token;
    const auto append_word = [&]() {
        if (token.empty() || output.size() >= maximum_tokens) return;
        if (vocabulary.contains(token)) {
            output.push_back(token);
            token.clear();
            return;
        }
        std::size_t start = 0;
        bool failed = false;
        while (start < token.size() && output.size() < maximum_tokens) {
            std::size_t end = token.size();
            std::string match;
            while (start < end) {
                std::string candidate = token.substr(start, end - start);
                if (start > 0) candidate = "##" + candidate;
                if (vocabulary.contains(candidate)) {
                    match = std::move(candidate);
                    break;
                }
                --end;
            }
            if (match.empty()) {
                failed = true;
                break;
            }
            output.push_back(std::move(match));
            start = end;
        }
        if (failed && output.size() < maximum_tokens) output.emplace_back("[UNK]");
        token.clear();
    };
    for (const auto character : text) {
        if (output.size() >= maximum_tokens) break;
        const auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        if (std::isspace(static_cast<unsigned char>(lower))) {
            append_word();
        } else if (std::ispunct(static_cast<unsigned char>(lower))) {
            append_word();
            if (output.size() < maximum_tokens) output.emplace_back(1, lower);
        } else {
            token.push_back(lower);
        }
    }
    if (output.size() < maximum_tokens) append_word();
    return output;
}

std::vector<std::int64_t> encode_ids(
    const std::string& text,
    const std::unordered_map<std::string, std::int64_t>& vocabulary,
    const std::size_t max_tokens
) {
    const auto pad_id = vocabulary.at("[PAD]");
    const auto cls_id = vocabulary.at("[CLS]");
    const auto sep_id = vocabulary.at("[SEP]");
    std::vector<std::int64_t> ids;
    ids.reserve(max_tokens);
    ids.push_back(cls_id);
    for (const auto& token : wordpiece_tokens(text, vocabulary, max_tokens - 2)) {
        if (ids.size() + 1 >= max_tokens) break;
        ids.push_back(vocabulary.contains(token) ? vocabulary.at(token) : vocabulary.at("[UNK]"));
    }
    ids.push_back(sep_id);
    ids.resize(max_tokens, pad_id);
    return ids;
}

std::string sha256(const std::string& value) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
}
#endif

FinbertSession::FinbertSession(std::string model_path, std::string vocab_path, std::string model_version,
                               std::string tokenizer_version, std::size_t max_tokens)
    : impl_(std::make_unique<Impl>(std::move(model_path), std::move(vocab_path), std::move(model_version), std::move(tokenizer_version), max_tokens)) {
    if (const auto* required = std::getenv("ARRAKIS_FINBERT_REQUIRE_EMBEDDING");
        required != nullptr && std::string_view{required} == "1") {
        impl_->require_embedding_output = true;
    }
    if (impl_->model_path.empty() || impl_->vocab_path.empty()) throw std::invalid_argument("FinBERT model and vocabulary paths are required");
    if (!std::filesystem::exists(impl_->model_path) || !std::filesystem::exists(impl_->vocab_path)) throw std::runtime_error("FinBERT ONNX artifact or tokenizer vocabulary is missing");
#if defined(ARRAKIS_HAS_ONNXRUNTIME)
    std::ifstream vocabulary_file(impl_->vocab_path);
    std::string token; std::int64_t id = 0;
    while (std::getline(vocabulary_file, token)) { if (!token.empty()) impl_->vocabulary.emplace(token, id); ++id; }
    if (!impl_->vocabulary.contains("[CLS]") || !impl_->vocabulary.contains("[SEP]") || !impl_->vocabulary.contains("[PAD]")) throw std::runtime_error("FinBERT vocabulary is missing required special tokens");
    auto thread_count = std::thread::hardware_concurrency();
    if (const auto* configured_threads = std::getenv("ARRAKIS_FINBERT_THREADS");
        configured_threads != nullptr && *configured_threads != '\0') {
        thread_count = static_cast<unsigned int>(std::stoul(configured_threads));
    }
    if (thread_count == 0) thread_count = 1;
    auto inter_op_threads = 1U;
    if (const auto* configured_inter_threads = std::getenv("ARRAKIS_FINBERT_INTER_THREADS");
        configured_inter_threads != nullptr && *configured_inter_threads != '\0') {
        inter_op_threads = static_cast<unsigned int>(std::stoul(configured_inter_threads));
    }
    if (inter_op_threads == 0) inter_op_threads = 1;
    impl_->options.SetIntraOpNumThreads(static_cast<int>(thread_count));
    impl_->options.SetInterOpNumThreads(static_cast<int>(inter_op_threads));
    auto execution_mode = ExecutionMode::ORT_SEQUENTIAL;
    if (const auto* configured_execution = std::getenv("ARRAKIS_FINBERT_EXECUTION_MODE");
        configured_execution != nullptr && std::string_view{configured_execution} == "parallel") {
        execution_mode = ExecutionMode::ORT_PARALLEL;
    }
    impl_->options.SetExecutionMode(execution_mode);
    auto graph_optimization = GraphOptimizationLevel::ORT_ENABLE_BASIC;
    if (const auto* configured_optimization = std::getenv("ARRAKIS_FINBERT_GRAPH_OPT");
        configured_optimization != nullptr && std::string_view{configured_optimization} == "all") {
        graph_optimization = GraphOptimizationLevel::ORT_ENABLE_ALL;
    } else if (configured_optimization != nullptr &&
               std::string_view{configured_optimization} == "extended") {
        graph_optimization = GraphOptimizationLevel::ORT_ENABLE_EXTENDED;
    }
    impl_->options.SetGraphOptimizationLevel(graph_optimization);
    if (const auto* coreml = std::getenv("ARRAKIS_FINBERT_COREML");
        coreml != nullptr && std::string_view{coreml} == "1") {
#if defined(ARRAKIS_HAS_COREML_PROVIDER)
        auto coreml_flags = static_cast<std::uint32_t>(COREML_FLAG_USE_CPU_AND_GPU);
        if (const auto* mode = std::getenv("ARRAKIS_FINBERT_COREML_MODE");
            mode != nullptr && std::string_view{mode} == "ane") {
            coreml_flags = static_cast<std::uint32_t>(COREML_FLAG_ONLY_ENABLE_DEVICE_WITH_ANE);
        } else if (mode != nullptr && std::string_view{mode} == "gpu-static") {
            coreml_flags |= static_cast<std::uint32_t>(COREML_FLAG_ONLY_ALLOW_STATIC_INPUT_SHAPES);
        }
        auto* status = OrtSessionOptionsAppendExecutionProvider_CoreML(
            impl_->options, coreml_flags
        );
        if (status != nullptr) {
            const std::string message = Ort::GetApi().GetErrorMessage(status);
            Ort::GetApi().ReleaseStatus(status);
            throw std::runtime_error{"Could not enable FinBERT CoreML execution provider: " + message};
        }
#else
        throw std::runtime_error{
            "ARRAKIS_FINBERT_COREML=1 requested, but this build has no CoreML provider"
        };
#endif
    }
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

std::string FinbertSession::token_input_hash(const std::string& text) const {
    if (!ready()) throw std::runtime_error("FinBERT session is not ready");
#if defined(ARRAKIS_HAS_ONNXRUNTIME)
    const auto ids = encode_ids(text, impl_->vocabulary, impl_->max_tokens);
    const auto pad_id = impl_->vocabulary.at("[PAD]");
    std::ostringstream key;
    key << impl_->model_version << '|' << impl_->tokenizer_version << '|'
        << impl_->max_tokens << '|';
    for (const auto id : ids) key << id << ',';
    key << '|';
    for (const auto id : ids) key << (id == pad_id ? 0 : 1) << ',';
    return sha256(key.str());
#else
    static_cast<void>(text);
    throw std::runtime_error("FinBERT ONNX Runtime support is unavailable");
#endif
}

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
    const auto pad_id = impl_->vocabulary.at("[PAD]");
    for (std::size_t row = 0; row < texts.size(); ++row) {
        ids[row] = encode_ids(texts[row], impl_->vocabulary, impl_->max_tokens);
        masks[row].assign(impl_->max_tokens, 0);
        types[row].assign(impl_->max_tokens, 0);
        for (std::size_t index = 0; index < impl_->max_tokens && ids[row][index] != pad_id; ++index) masks[row][index] = 1;
    }
    std::vector<std::int64_t> flat_ids, flat_masks, flat_types; for (std::size_t row = 0; row < texts.size(); ++row) { flat_ids.insert(flat_ids.end(), ids[row].begin(), ids[row].end()); flat_masks.insert(flat_masks.end(), masks[row].begin(), masks[row].end()); flat_types.insert(flat_types.end(), types[row].begin(), types[row].end()); }
    const std::array<std::int64_t, 2> shape{static_cast<std::int64_t>(texts.size()), static_cast<std::int64_t>(impl_->max_tokens)};
    Ort::MemoryInfo memory_info("Cpu", OrtDeviceAllocator, 0, OrtMemTypeDefault);
    std::vector<Ort::Value> inputs;
    inputs.push_back(Ort::Value::CreateTensor<std::int64_t>(memory_info, flat_ids.data(), flat_ids.size(), shape.data(), shape.size()));
    inputs.push_back(Ort::Value::CreateTensor<std::int64_t>(memory_info, flat_masks.data(), flat_masks.size(), shape.data(), shape.size()));
    if (input_count > 2) inputs.push_back(Ort::Value::CreateTensor<std::int64_t>(memory_info, flat_types.data(), flat_types.size(), shape.data(), shape.size()));
    std::vector<const char*> input_names; for (const auto& name : input_names_storage) input_names.push_back(name.c_str());
    const auto output_count = impl_->session->GetOutputCount(); std::vector<std::string> output_names_storage; output_names_storage.reserve(output_count); std::vector<const char*> output_names; output_names.reserve(output_count); for (std::size_t index = 0; index < output_count; ++index) { output_names_storage.emplace_back(impl_->session->GetOutputNameAllocated(index, allocator).get()); output_names.push_back(output_names_storage.back().c_str()); }
    const auto outputs = impl_->session->Run(Ort::RunOptions{nullptr}, input_names.data(), inputs.data(), inputs.size(), output_names.data(), output_names.size());
    if (outputs.empty()) throw std::runtime_error("FinBERT ONNX model returned no outputs");
    const auto* logits = outputs[0].GetTensorData<float>(); const auto logits_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape(); if (logits == nullptr || logits_shape.size() != 2 || logits_shape[0] != static_cast<std::int64_t>(texts.size()) || logits_shape[1] < 3) throw std::runtime_error("FinBERT logits output has an invalid shape");
    std::vector<FinbertOutput> result(texts.size()); for (std::size_t row = 0; row < texts.size(); ++row) { const double max_logit = std::max({static_cast<double>(logits[row * 3]), static_cast<double>(logits[row * 3 + 1]), static_cast<double>(logits[row * 3 + 2])}); double sum = 0; double probabilities[3]{}; for (std::size_t index = 0; index < 3; ++index) { probabilities[index] = std::exp(static_cast<double>(logits[row * 3 + index]) - max_logit); sum += probabilities[index]; } result[row].positive_probability = probabilities[0] / sum; result[row].negative_probability = probabilities[1] / sum; result[row].neutral_probability = probabilities[2] / sum; result[row].sentiment_score = result[row].positive_probability - result[row].negative_probability; }
    if (impl_->require_embedding_output && output_count < 2) {
        throw std::runtime_error{
            "FinBERT embedding output is required but the ONNX graph exposes logits only"
        };
    }
    if (output_count > 1) {
        const auto* embeddings = outputs[1].GetTensorData<float>();
        const auto embedding_shape = outputs[1].GetTensorTypeAndShapeInfo().GetShape();
        const bool valid_embedding_shape =
            embeddings != nullptr && embedding_shape.size() == 2 &&
            embedding_shape[0] == static_cast<std::int64_t>(texts.size()) &&
            embedding_shape[1] > 0;
        if (!valid_embedding_shape && impl_->require_embedding_output) {
            throw std::runtime_error{"FinBERT embedding output has an invalid shape"};
        }
        if (valid_embedding_shape) {
            for (std::size_t row = 0; row < texts.size(); ++row) {
                const auto width = static_cast<std::size_t>(embedding_shape[1]);
                result[row].pooled_embedding.assign(
                    embeddings + row * width, embeddings + (row + 1) * width
                );
            }
        }
    }
    return result;
#else
    static_cast<void>(texts);
    throw std::runtime_error("FinBERT ONNX Runtime support is unavailable");
#endif
}

}  // namespace arrakis::news
