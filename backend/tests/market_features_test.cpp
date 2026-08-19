#include "arrakis/news/market_features.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

std::string date_for(const int month, const int index) {
    return std::string("2020-") + (month < 10 ? "0" : "") + std::to_string(month) + "-" +
        (index < 9 ? "0" : "") + std::to_string(index + 1);
}

}  // namespace

TEST(MarketFeatureGoldenTest, ComputesVolatilityAndRsiWithoutProxyFormulas) {
    std::vector<arrakis::news::MarketDay> xlk_days;
    std::vector<arrakis::news::MarketDay> spy_days;
    for (int index = 0; index <= 14; ++index) {
        const auto date = date_for(1, index);
        const auto xlk_close = index == 14 ? 110.0 : 100.0;
        xlk_days.push_back({date, xlk_close, 1000.0});
        spy_days.push_back({date, 200.0, 2000.0});
    }

    const auto result = arrakis::news::market_feature_vector(xlk_days, spy_days, "2020-01-15");
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 9U);
    EXPECT_DOUBLE_EQ((*result)[6], 100.0);
    const auto last_log_return = std::log(1.1);
    const auto mean = last_log_return / 6.0;
    const auto expected_volatility = std::sqrt(
        ((last_log_return - mean) * (last_log_return - mean) + 5.0 * mean * mean) / 5.0);
    EXPECT_NEAR((*result)[3], expected_volatility, 1.0e-12);
    EXPECT_NE((*result)[6], std::clamp(50.0 + 10.0 * (*result)[0], 0.0, 100.0));
}

TEST(MarketFeatureGoldenTest, ConstantPricesProduceNeutralRsiAndZeroVolatility) {
    std::vector<arrakis::news::MarketDay> xlk_days;
    std::vector<arrakis::news::MarketDay> spy_days;
    for (int index = 0; index <= 14; ++index) {
        const auto date = date_for(2, index);
        xlk_days.push_back({date, 100.0, 1000.0});
        spy_days.push_back({date, 200.0, 2000.0});
    }
    const auto result = arrakis::news::market_feature_vector(xlk_days, spy_days, "2020-02-15");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ((*result)[3], 0.0);
    EXPECT_DOUBLE_EQ((*result)[6], 50.0);
}
