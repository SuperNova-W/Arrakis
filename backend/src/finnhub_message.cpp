#include "arrakis/market/finnhub_message.hpp"

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace arrakis::market {
namespace {

[[nodiscard]] const boost::json::value& required_value(
    const boost::json::object& object,
    std::string_view key
) {
    const auto* value = object.if_contains(key);
    if (value == nullptr) {
        throw std::runtime_error("Finnhub message is missing required field: " + std::string(key));
    }
    return *value;
}

[[nodiscard]] std::string required_string(
    const boost::json::object& object,
    std::string_view key
) {
    const auto& value = required_value(object, key);
    if (!value.is_string()) {
        throw std::runtime_error("Finnhub field is not a string: " + std::string(key));
    }
    return std::string(value.as_string());
}

[[nodiscard]] double required_number(
    const boost::json::object& object,
    std::string_view key
) {
    const auto& value = required_value(object, key);
    if (!value.is_number()) {
        throw std::runtime_error("Finnhub field is not numeric: " + std::string(key));
    }
    return value.to_number<double>();
}

[[nodiscard]] std::uint64_t required_timestamp(
    const boost::json::object& object,
    std::string_view key
) {
    const auto& value = required_value(object, key);
    if (value.is_uint64()) {
        return value.as_uint64();
    }
    if (value.is_int64() && value.as_int64() >= 0) {
        return static_cast<std::uint64_t>(value.as_int64());
    }
    throw std::runtime_error("Finnhub timestamp is not a non-negative integer");
}

[[nodiscard]] std::vector<std::string> string_array(
    const boost::json::object& object,
    std::string_view key
) {
    std::vector<std::string> result;
    const auto* value = object.if_contains(key);
    if (value == nullptr) {
        return result;
    }
    if (!value->is_array()) {
        throw std::runtime_error("Finnhub field is not an array: " + std::string(key));
    }
    for (const auto& item : value->as_array()) {
        if (!item.is_string()) {
            throw std::runtime_error("Finnhub array contains a non-string: " + std::string(key));
        }
        result.emplace_back(item.as_string());
    }
    return result;
}

[[nodiscard]] TradeEvent parse_trade(const boost::json::object& object) {
    TradeEvent event;
    event.symbol = required_string(object, "s");
    event.price = required_number(object, "p");
    event.size = required_number(object, "v");
    event.timestamp_ms = required_timestamp(object, "t");
    event.conditions = string_array(object, "c");
    return event;
}

[[nodiscard]] ControlMessage parse_control(const boost::json::object& object) {
    const auto type = required_string(object, "type");
    ControlMessage control;
    if (type == "ping") {
        control.kind = ControlKind::ping;
    } else if (type == "error") {
        control.kind = ControlKind::error;
    }
    if (const auto* message = object.if_contains("msg"); message != nullptr) {
        if (!message->is_string()) {
            throw std::runtime_error("Finnhub field is not a string: msg");
        }
        control.message = std::string(message->as_string());
    }
    return control;
}

}  // namespace

FinnhubFrame parse_finnhub_frame(std::string_view payload) {
    boost::system::error_code error;
    const auto document = boost::json::parse(payload, error);
    if (error) {
        throw std::runtime_error("Invalid JSON from Finnhub: " + error.message());
    }
    if (!document.is_object()) {
        throw std::runtime_error("Finnhub WebSocket frame must contain a JSON object");
    }

    const auto& object = document.as_object();
    const auto type = required_string(object, "type");
    FinnhubFrame frame;
    if (type == "trade") {
        const auto* data = object.if_contains("data");
        if (data == nullptr || !data->is_array()) {
            throw std::runtime_error("Finnhub trade frame must contain a data array");
        }
        for (const auto& item : data->as_array()) {
            if (!item.is_object()) {
                throw std::runtime_error("Finnhub trade data contains a non-object");
            }
            frame.trades.push_back(parse_trade(item.as_object()));
        }
    } else {
        frame.controls.push_back(parse_control(object));
    }
    return frame;
}

std::string trade_event_to_json(const TradeEvent& event) {
    boost::json::array conditions;
    conditions.reserve(event.conditions.size());
    for (const auto& condition : event.conditions) {
        conditions.emplace_back(condition);
    }

    return boost::json::serialize(boost::json::object{
        {"event_type", "trade"},
        {"source", "finnhub"},
        {"symbol", event.symbol},
        {"price", event.price},
        {"size", event.size},
        {"timestamp_ms", event.timestamp_ms},
        {"conditions", std::move(conditions)},
    });
}

}  // namespace arrakis::market
