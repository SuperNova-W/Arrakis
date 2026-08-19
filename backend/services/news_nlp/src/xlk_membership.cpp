#include "arrakis/news/xlk_membership.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace arrakis::news {
namespace {

constexpr std::string_view kSentinelEffectiveTo = "2099-12-31";

[[nodiscard]] std::string trim(std::string value) {
    const auto is_space = [](const unsigned char character) { return std::isspace(character) != 0; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) value.pop_back();
    std::size_t start = 0;
    while (start < value.size() && is_space(static_cast<unsigned char>(value[start]))) ++start;
    return value.substr(start);
}

[[nodiscard]] std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    std::istringstream input{line};
    while (std::getline(input, field, ',')) fields.push_back(trim(std::move(field)));
    if (!line.empty() && line.back() == ',') fields.emplace_back();
    return fields;
}

[[nodiscard]] bool is_iso_date(std::string_view value) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 4 || index == 7) continue;
        if (std::isdigit(static_cast<unsigned char>(value[index])) == 0) return false;
    }
    return true;
}

// ISO dates compare correctly lexicographically, so no calendar arithmetic is
// needed anywhere in this file.
[[nodiscard]] std::vector<MembershipSnapshot> parse(std::istream& input, const std::string& origin) {
    std::string line;
    if (!std::getline(input, line)) {
        throw MembershipDataError{"Holdings history is empty: " + origin};
    }
    const auto header = split_csv_line(line);
    if (header.size() < 4 || header[0] != "symbol" || header[1] != "effective_from" ||
        header[2] != "effective_to" || header[3] != "weight") {
        throw MembershipDataError{
            "Holdings history must start with symbol,effective_from,effective_to,weight: " + origin};
    }

    // effective_from -> symbol -> (weight, source). std::map keeps both levels
    // sorted, which makes the resolver's output order deterministic.
    std::map<std::string, std::map<std::string, std::pair<double, std::string>>> grouped;
    std::map<std::string, std::string> snapshot_source;
    std::size_t row_number = 1;
    while (std::getline(input, line)) {
        ++row_number;
        if (trim(line).empty()) continue;
        const auto fields = split_csv_line(line);
        // The shipped file was assembled by concatenation and repeats its
        // header near the end (row 955), followed by a duplicate of the first
        // AAPL row. Skip exact header repeats; anything else malformed throws.
        if (fields == header) continue;
        const auto where = " (" + origin + " row " + std::to_string(row_number) + ")";
        if (fields.size() < 4) throw MembershipDataError{"Malformed holdings row" + where};
        const auto& symbol = fields[0];
        const auto& effective_from = fields[1];
        const auto& effective_to = fields[2];
        if (symbol.empty()) throw MembershipDataError{"Empty symbol" + where};
        if (!is_iso_date(effective_from)) {
            throw MembershipDataError{"Invalid effective_from '" + effective_from + "'" + where};
        }
        if (!is_iso_date(effective_to)) {
            throw MembershipDataError{"Invalid effective_to '" + effective_to + "'" + where};
        }
        // effective_to is deliberately not used for membership. Anything other
        // than the known sentinel means the file's meaning changed, so refuse
        // rather than silently ignore a column that now carries information.
        if (effective_to != kSentinelEffectiveTo) {
            throw MembershipDataError{
                "effective_to is expected to be the constant sentinel " +
                std::string{kSentinelEffectiveTo} + " and is ignored for membership, but found '" +
                effective_to + "'" + where +
                "; update the resolver policy before trusting this column"};
        }
        double weight = 0.0;
        try {
            weight = std::stod(fields[3]);
        } catch (const std::exception&) {
            throw MembershipDataError{"Invalid weight '" + fields[3] + "'" + where};
        }
        if (!(weight >= 0.0)) throw MembershipDataError{"Negative or NaN weight" + where};

        const std::string source = fields.size() > 4 ? fields[4] : std::string{};
        auto& snapshot = grouped[effective_from];
        const auto existing = snapshot.find(symbol);
        if (existing != snapshot.end()) {
            // The published file contains an exact duplicate of the first AAPL
            // row. Identical duplicates are harmless; conflicting ones are not.
            if (existing->second.first != weight) {
                throw MembershipDataError{
                    "Conflicting weights for " + symbol + " on " + effective_from + where};
            }
            continue;
        }
        snapshot.emplace(symbol, std::pair{weight, source});
        if (!source.empty()) snapshot_source.try_emplace(effective_from, source);
    }

    if (grouped.empty()) throw MembershipDataError{"Holdings history has no rows: " + origin};

    std::vector<MembershipSnapshot> snapshots;
    snapshots.reserve(grouped.size());
    for (const auto& [effective_from, members] : grouped) {
        MembershipSnapshot snapshot;
        snapshot.effective_from = effective_from;
        if (const auto found = snapshot_source.find(effective_from); found != snapshot_source.end()) {
            snapshot.source = found->second;
        }
        snapshot.constituents.reserve(members.size());
        for (const auto& [symbol, payload] : members) {
            snapshot.constituents.push_back(ConstituentWeight{symbol, payload.first});
        }
        snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
}

const std::vector<ConstituentWeight>& empty_constituents() {
    static const std::vector<ConstituentWeight> value;
    return value;
}

}  // namespace

XlkMembershipResolver::XlkMembershipResolver(std::vector<MembershipSnapshot> snapshots)
    : snapshots_{std::move(snapshots)} {}

XlkMembershipResolver XlkMembershipResolver::from_csv(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw MembershipDataError{"Could not open holdings history: " + path.string()};
    return XlkMembershipResolver{parse(input, path.string())};
}

XlkMembershipResolver XlkMembershipResolver::from_csv_text(std::string_view text) {
    std::istringstream input{std::string{text}};
    return XlkMembershipResolver{parse(input, "<memory>")};
}

const std::vector<ConstituentWeight>& XlkMembershipResolver::constituents_on(
    const std::string_view date
) const {
    // Last snapshot whose effective_from <= date. Everything earlier has been
    // superseded; everything later has not happened yet at `date`.
    const auto upper = std::upper_bound(
        snapshots_.begin(), snapshots_.end(), date,
        [](const std::string_view value, const MembershipSnapshot& snapshot) {
            return value < snapshot.effective_from;
        }
    );
    if (upper == snapshots_.begin()) return empty_constituents();
    return std::prev(upper)->constituents;
}

std::optional<double> XlkMembershipResolver::weight_on(
    const std::string_view symbol,
    const std::string_view date
) const {
    const auto& members = constituents_on(date);
    const auto found = std::lower_bound(
        members.begin(), members.end(), symbol,
        [](const ConstituentWeight& left, const std::string_view value) {
            return left.symbol < value;
        }
    );
    if (found == members.end() || found->symbol != symbol) return std::nullopt;
    return found->weight_percent;
}

bool XlkMembershipResolver::held_on(const std::string_view symbol, const std::string_view date) const {
    return weight_on(symbol, date).has_value();
}

bool XlkMembershipResolver::held_strictly_before(
    const std::string_view symbol,
    const std::string_view date
) const {
    const auto upper = std::lower_bound(
        snapshots_.begin(), snapshots_.end(), date,
        [](const MembershipSnapshot& snapshot, const std::string_view value) {
            return snapshot.effective_from < value;
        }
    );
    if (upper == snapshots_.begin()) return false;
    const auto& members = std::prev(upper)->constituents;
    const auto found = std::lower_bound(
        members.begin(), members.end(), symbol,
        [](const ConstituentWeight& left, const std::string_view value) {
            return left.symbol < value;
        }
    );
    return found != members.end() && found->symbol == symbol;
}

std::optional<std::string> XlkMembershipResolver::governing_snapshot(const std::string_view date) const {
    const auto upper = std::upper_bound(
        snapshots_.begin(), snapshots_.end(), date,
        [](const std::string_view value, const MembershipSnapshot& snapshot) {
            return value < snapshot.effective_from;
        }
    );
    if (upper == snapshots_.begin()) return std::nullopt;
    return std::prev(upper)->effective_from;
}

std::optional<std::string> XlkMembershipResolver::governing_snapshot_strictly_before(
    const std::string_view date
) const {
    const auto upper = std::lower_bound(
        snapshots_.begin(), snapshots_.end(), date,
        [](const MembershipSnapshot& snapshot, const std::string_view value) {
            return snapshot.effective_from < value;
        }
    );
    if (upper == snapshots_.begin()) return std::nullopt;
    return std::prev(upper)->effective_from;
}

bool XlkMembershipResolver::is_extrapolated_forward(const std::string_view date) const {
    return date >= last_snapshot_date();
}

const std::string& XlkMembershipResolver::first_snapshot_date() const {
    return snapshots_.front().effective_from;
}

const std::string& XlkMembershipResolver::last_snapshot_date() const {
    return snapshots_.back().effective_from;
}

std::filesystem::path XlkMembershipResolver::default_history_path() {
    if (const char* override_path = std::getenv("XLK_HOLDINGS_HISTORY");
        override_path != nullptr && *override_path != '\0') {
        return std::filesystem::path{override_path};
    }
    // Walk up from the working directory so the same default works from the
    // backend root, from a build/<config> directory and from an install prefix.
    const std::filesystem::path relative{"data/metadata/xlk_holdings_history.csv"};
    std::filesystem::path prefix{"."};
    for (int level = 0; level < 5; ++level) {
        std::error_code error;
        const auto candidate = prefix / relative;
        if (std::filesystem::exists(candidate, error)) return candidate.lexically_normal();
        prefix /= "..";
    }
    return relative;
}

bool is_pollable_ticker(const std::string_view symbol) {
    if (symbol.empty() || symbol.size() > 5) return false;
    return std::all_of(symbol.begin(), symbol.end(), [](const char character) {
        return character >= 'A' && character <= 'Z';
    });
}

}  // namespace arrakis::news
