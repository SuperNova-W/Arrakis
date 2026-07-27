#include "arrakis/prediction/prediction_service.hpp"
#include "arrakis/model/sector_ml.hpp"
#include "feature_event.pb.h"
#include "model_prediction.pb.h"

#include <gtest/gtest.h>

#include <cstring>
#include <cmath>
#include <filesystem>
#include <limits>

namespace {
std::vector<std::byte> encode(const market::features::v1::FeatureEvent& input) {
    std::string bytes; EXPECT_TRUE(input.SerializeToString(&bytes));
    std::vector<std::byte> result(bytes.size()); std::memcpy(result.data(), bytes.data(), bytes.size()); return result;
}
void expect_invalid(const std::vector<std::byte>& bytes) {
    EXPECT_THROW({ const auto ignored = arrakis::prediction::deserialize_feature(bytes); static_cast<void>(ignored); }, std::invalid_argument);
}
}

TEST(PredictionSerializationTest, RejectsIncompleteOrUnversionedFeatureEvent) {
    market::features::v1::FeatureEvent event;
    event.set_event_id("feature-1"); event.set_target_symbol("XLK");
    expect_invalid(encode(event));
}

TEST(PredictionSerializationTest, DeserializesNamedFiniteFeatures) {
    market::features::v1::FeatureEvent event;
    event.set_event_id("feature-1"); event.set_target_symbol("XLK"); event.set_event_time_unix_ms(1234);
    event.set_feature_version("sector-features-v1"); event.set_feature_schema_hash("abc"); event.set_complete_context(true);
    auto* feature = event.add_features(); feature->set_name("return_1"); feature->set_value(0.25);
    const auto output = arrakis::prediction::deserialize_feature(encode(event));
    EXPECT_EQ(output.event_id, "feature-1"); EXPECT_EQ(output.target_symbol, "XLK"); EXPECT_EQ(output.names, std::vector<std::string>{"return_1"}); EXPECT_FLOAT_EQ(output.values.front(), 0.25F);
}

TEST(PredictionSerializationTest, RejectsNonFiniteFeature) {
    market::features::v1::FeatureEvent event;
    event.set_event_id("feature-1"); event.set_target_symbol("XLK"); event.set_feature_version("sector-features-v1"); event.set_feature_schema_hash("abc"); event.set_complete_context(true);
    auto* feature = event.add_features(); feature->set_name("return_1"); feature->set_value(std::numeric_limits<double>::quiet_NaN());
    expect_invalid(encode(event));
}

TEST(PredictionIntegrationTest, LoadsArtifactAndPublishesPredictionEvent) {
    std::vector<arrakis::model::BarRecord> bars;
    for (int index = 0; index < 32; ++index) {
        const auto date = "2024-01-" + std::to_string(1 + index);
        bars.push_back({date, "XLK", 100.0 + index, 101.0 + index, 99.0 + index, 100.0 + index, 1000.0 + index});
        bars.push_back({date, "SPY", 200.0 + index, 201.0 + index, 199.0 + index, 200.0 + index, 1200.0 + index});
    }
    arrakis::model::FeatureConfiguration training_config;
    training_config.lookback_window = 6; training_config.prediction_horizon_bars = 2; training_config.max_rounds = 3;
    arrakis::model::SectorFeatureBuilder builder(training_config);
    const auto dataset = builder.build(bars);
    const auto directory = std::filesystem::temp_directory_path() / "arrakis_prediction_service_test";
    std::filesystem::remove_all(directory);
    arrakis::model::SectorXGBoostTrainer trainer(training_config);
    const auto model_path = trainer.train(dataset, directory);
    ASSERT_EQ(model_path, directory / "model.ubj");

    arrakis::prediction::ServiceConfig config;
    config.model_path = directory / "model.ubj"; config.metadata_path = directory / "metadata.json"; config.schema_path = directory / "feature_schema.json";
    arrakis::runtime::Metrics metrics;
    arrakis::prediction::PredictionService service(config, metrics);
    market::features::v1::FeatureEvent event;
    event.set_event_id("feature-1"); event.set_target_symbol("XLK"); event.set_event_time_unix_ms(1234); event.set_feature_version("sector-features-v1"); event.set_feature_schema_hash("runtime-hash"); event.set_complete_context(true);
    for (std::size_t index = 0; index < dataset.feature_names.size(); ++index) { auto* item = event.add_features(); item->set_name(dataset.feature_names[index]); item->set_value(dataset.features[index]); }
    const auto output_bytes = service.predict(encode(event));
    model::predictions::v1::PredictionEvent output;
    ASSERT_TRUE(output.ParseFromArray(output_bytes.data(), static_cast<int>(output_bytes.size())));
    EXPECT_EQ(output.feature_event_id(), "feature-1"); EXPECT_EQ(output.target_symbol(), "XLK"); EXPECT_TRUE(std::isfinite(output.predicted_return()));
}
