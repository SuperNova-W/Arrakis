#include "arrakis/market/normalization.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace arrakis::market {
namespace {
std::string normalized_symbol(std::string_view input) {
    std::size_t first = 0;
    while (first < input.size() && std::isspace(static_cast<unsigned char>(input[first])) != 0) ++first;
    std::size_t last = input.size();
    while (last > first && std::isspace(static_cast<unsigned char>(input[last - 1])) != 0) --last;
    std::string result(input.substr(first, last - first));
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}
std::string number(double value) { std::ostringstream stream; stream << std::setprecision(15) << value; return stream.str(); }
std::string sha256_hex(std::string_view value) {
    unsigned char digest[SHA256_DIGEST_LENGTH]{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
}
}

NormalizedTrade normalize_trade(const TradeEvent& trade, std::string_view source, std::size_t sequence) {
    const auto symbol = normalized_symbol(trade.symbol);
    if (symbol.empty()) throw std::runtime_error("trade symbol is empty");
    if (!std::isfinite(trade.price) || trade.price <= 0.0) throw std::runtime_error("trade price must be positive and finite");
    if (!std::isfinite(trade.size) || trade.size < 0.0) throw std::runtime_error("trade volume must be non-negative and finite");
    if (trade.timestamp_ms == 0U) throw std::runtime_error("trade timestamp is invalid");
    NormalizedTrade result;
    result.source = std::string(source); result.symbol = symbol; result.price = trade.price; result.volume = trade.size;
    result.source_timestamp_unix_ms = trade.timestamp_ms; result.conditions = trade.conditions;
    std::ostringstream canonical;
    canonical << result.source << '|' << result.symbol << '|' << result.source_timestamp_unix_ms << '|'
              << number(result.price) << '|' << number(result.volume) << '|' << sequence;
    for (const auto& condition : result.conditions) canonical << '|' << condition;
    result.event_id = result.source + ":sha256:" + sha256_hex(canonical.str());
    return result;
}
}  // namespace arrakis::market
