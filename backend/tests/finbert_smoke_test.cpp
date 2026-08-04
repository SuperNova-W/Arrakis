#include "arrakis/news/finbert.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

TEST(FinbertSmokeTest, RunsLocalOnnxArtifactWhenConfigured) {
    const char* model = std::getenv("ARRAKIS_FINBERT_ONNX_PATH");
    const char* vocab = std::getenv("ARRAKIS_FINBERT_VOCAB_PATH");
    if (model == nullptr || vocab == nullptr) GTEST_SKIP() << "FinBERT artifacts are not configured";
    arrakis::news::FinbertSession session(model, vocab, "finbert-v1", "finbert-tokenizer-v1");
    const auto outputs = session.infer({"Technology companies report resilient cloud demand."});
    ASSERT_EQ(outputs.size(), 1);
    EXPECT_NEAR(outputs[0].positive_probability + outputs[0].neutral_probability + outputs[0].negative_probability, 1.0, 1e-5);
}
