#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace arrakis::news {

struct FinbertOutput final {
    double positive_probability{};
    double neutral_probability{};
    double negative_probability{};
    double sentiment_score{};
    std::vector<double> pooled_embedding;
};

class FinbertSession final {
public:
    FinbertSession(std::string model_path, std::string vocab_path, std::string model_version,
                   std::string tokenizer_version, std::size_t max_tokens = 128);
    ~FinbertSession();
    FinbertSession(const FinbertSession&) = delete;
    FinbertSession& operator=(const FinbertSession&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] const std::string& model_version() const noexcept;
    [[nodiscard]] const std::string& tokenizer_version() const noexcept;
    [[nodiscard]] std::vector<FinbertOutput> infer(const std::vector<std::string>& texts) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace arrakis::news
