#pragma once

#include "arrakis/news/feature_schema.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

namespace arrakis::news {

struct MarketDay final {
    std::string date;
    double close{};
    double volume{};
};

[[nodiscard]] inline std::optional<std::vector<double>> market_feature_vector(
    const std::vector<MarketDay>& xlk_days, const std::vector<MarketDay>& spy_days,
    std::string_view date) {
    const auto current_it = std::find_if(xlk_days.begin(), xlk_days.end(), [&](const auto& day) {
        return day.date == date;
    });
    if (current_it == xlk_days.end()) return std::nullopt;
    const auto current_index = static_cast<std::size_t>(std::distance(xlk_days.begin(), current_it));
    if (current_index < 14) return std::nullopt;

    const auto spy_current_it = std::find_if(spy_days.begin(), spy_days.end(), [&](const auto& day) {
        return day.date == date;
    });
    const auto previous_date = xlk_days[current_index - 1].date;
    const auto spy_previous_it = std::find_if(spy_days.begin(), spy_days.end(), [&](const auto& day) {
        return day.date == previous_date;
    });
    if (spy_current_it == spy_days.end() || spy_previous_it == spy_days.end()) return std::nullopt;

    const auto& current = *current_it;
    const auto& previous_one = xlk_days[current_index - 1];
    const auto& previous_three = xlk_days[current_index - 3];
    const auto& previous_six = xlk_days[current_index - 6];
    const double ret_1 = current.close / previous_one.close - 1.0;
    const double ret_3 = current.close / previous_three.close - 1.0;
    const double ret_6 = current.close / previous_six.close - 1.0;
    double volume_sum = 0.0;
    for (std::size_t index = current_index - 5; index <= current_index; ++index) {
        volume_sum += xlk_days[index].volume;
    }
    const double volume_mean = volume_sum / 6.0;
    const double spy_ret_1 = spy_current_it->close / spy_previous_it->close - 1.0;
    std::vector<double> recent_log_returns;
    recent_log_returns.reserve(6);
    for (std::size_t index = current_index - 5; index <= current_index; ++index) {
        const auto previous_close = xlk_days[index - 1].close;
        const auto close = xlk_days[index].close;
        if (previous_close <= 0.0 || close <= 0.0) return std::nullopt;
        recent_log_returns.push_back(std::log(close / previous_close));
    }
    const auto return_mean = std::accumulate(recent_log_returns.begin(), recent_log_returns.end(), 0.0) /
        static_cast<double>(recent_log_returns.size());
    double squared_deviation = 0.0;
    for (const auto item : recent_log_returns) squared_deviation += (item - return_mean) * (item - return_mean);
    const double volatility_6 = std::sqrt(squared_deviation /
        static_cast<double>(recent_log_returns.size() - 1));

    double average_gain = 0.0;
    double average_loss = 0.0;
    for (std::size_t index = current_index - 13; index <= current_index; ++index) {
        const auto change = xlk_days[index].close - xlk_days[index - 1].close;
        average_gain += std::max(0.0, change);
        average_loss += std::max(0.0, -change);
    }
    average_gain /= 14.0;
    average_loss /= 14.0;
    const double rsi_14 = average_loss == 0.0
        ? (average_gain == 0.0 ? 50.0 : 100.0)
        : 100.0 - (100.0 / (1.0 + average_gain / average_loss));

    return std::vector<double>{
        ret_1,
        ret_3,
        ret_6,
        volatility_6,
        volume_mean,
        current.volume / std::max(1.0, volume_mean),
        rsi_14,
        spy_ret_1,
        ret_1 - spy_ret_1};
}

}  // namespace arrakis::news
