#include <boost/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

[[nodiscard]] std::vector<std::string> read_record(std::istream& input) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    char character = 0;
    while (input.get(character)) {
        if (character == '"') {
            if (quoted && input.peek() == '"') {
                input.get(character);
                field.push_back(character);
            } else {
                quoted = !quoted;
            }
        } else if (character == ',' && !quoted) {
            fields.push_back(field);
            field.clear();
        } else if ((character == '\n' || character == '\r') && !quoted) {
            if (character == '\r' && input.peek() == '\n') input.get(character);
            fields.push_back(field);
            return fields;
        } else {
            field.push_back(character);
        }
    }
    if (quoted) throw std::runtime_error{"CSV ended inside a quoted field"};
    if (!field.empty() || !fields.empty()) fields.push_back(field);
    return fields;
}

[[nodiscard]] std::size_t column_index(
    const std::vector<std::string>& header,
    const std::string_view name
) {
    const auto found = std::ranges::find(header, name);
    if (found == header.end()) throw std::runtime_error{"Missing column: " + std::string{name}};
    return static_cast<std::size_t>(std::distance(header.begin(), found));
}

[[nodiscard]] std::string csv_escape(const std::string& value) {
    if (value.find_first_of(",\"\n\r") == std::string::npos) return value;
    std::string escaped{"\""};
    for (const auto character : value) {
        escaped.push_back(character);
        if (character == '"') escaped.push_back('"');
    }
    escaped.push_back('"');
    return escaped;
}

[[nodiscard]] std::string json_string(const boost::json::object& object, const std::string_view name) {
    const auto* value = object.if_contains(name);
    return value != nullptr && value->is_string() ? std::string{value->as_string()} : std::string{};
}

[[nodiscard]] std::optional<double> json_number(const boost::json::object& object, const std::string_view name) {
    const auto* value = object.if_contains(name);
    if (value == nullptr) return std::nullopt;
    if (value->is_double()) return value->as_double();
    if (value->is_int64()) return static_cast<double>(value->as_int64());
    if (value->is_uint64()) return static_cast<double>(value->as_uint64());
    return std::nullopt;
}

[[nodiscard]] double clipped_change(const double current, const double previous) {
    if (!std::isfinite(current) || !std::isfinite(previous) || std::abs(previous) < 1.0e-12) return 0.0;
    return std::clamp(current / previous - 1.0, -5.0, 5.0);
}

struct Event final {
    std::string accession;
    std::string accepted_at;
    std::string trading_date;
    std::string sector;
    std::string symbol;
    std::string form;
};

struct Holding final {
    std::string sector;
    std::string symbol;
    std::string cik;
};

struct FactCandidate final {
    double value{};
    std::string end;
    int tag_priority{};
    bool has_frame{};
};

struct RawSnapshot final {
    std::string accession;
    std::string accepted_at;
    std::string trading_date;
    std::string sector;
    std::string symbol;
    std::optional<double> revenue;
    std::optional<double> net_income;
    std::optional<double> assets;
    std::optional<double> liabilities;
    std::optional<double> eps;
};

struct Snapshot final {
    std::string trading_date;
    std::string symbol;
    double revenue_growth{};
    double net_margin{};
    double assets_growth{};
    double liabilities_growth{};
    double eps_change{};
    bool has_revenue{};
    bool has_net_margin{};
    bool has_assets_growth{};
    bool has_liabilities_growth{};
    bool has_eps_change{};
};

struct Aggregate final {
    std::size_t filing_count{};
    std::size_t coverage{};
    double revenue_growth_sum{};
    std::size_t revenue_growth_count{};
    double net_margin_sum{};
    std::size_t net_margin_count{};
    double assets_growth_sum{};
    std::size_t assets_growth_count{};
    double liabilities_growth_sum{};
    std::size_t liabilities_growth_count{};
    double eps_change_sum{};
    std::size_t eps_change_count{};
};

[[nodiscard]] std::vector<Holding> load_holdings(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open holdings: " + path.string()};
    std::vector<Holding> result;
    while (true) {
        const auto row = read_record(input);
        if (row.empty()) break;
        if (row.size() < 4 || row[0] == "sector" || row[3].empty()) continue;
        result.push_back(Holding{.sector = row[0], .symbol = row[1], .cik = row[3]});
    }
    if (result.empty()) throw std::runtime_error{"Holdings contain no usable CIKs"};
    return result;
}

[[nodiscard]] std::vector<Event> load_events(
    const std::filesystem::path& path,
    std::unordered_map<std::string, Event>& by_accession
) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open SEC filing events"};
    const auto header = read_record(input);
    const auto accession = column_index(header, "accession");
    const auto accepted_at = column_index(header, "published_at_utc");
    const auto trading_date = column_index(header, "trading_date");
    const auto sector = column_index(header, "sector");
    const auto symbol = column_index(header, "symbol");
    const auto form = column_index(header, "form");
    std::vector<Event> result;
    while (true) {
        const auto row = read_record(input);
        if (row.empty()) break;
        if (row.size() != header.size()) throw std::runtime_error{"Malformed SEC event row"};
        const auto event = Event{
            .accession = row[accession], .accepted_at = row[accepted_at],
            .trading_date = row[trading_date], .sector = row[sector], .symbol = row[symbol],
            .form = row[form]
        };
        if (!by_accession.emplace(event.accession, event).second) {
            throw std::runtime_error{"Duplicate SEC accession: " + event.accession};
        }
        result.push_back(event);
    }
    return result;
}

[[nodiscard]] bool supported_fundamental_form(const std::string_view form) {
    return form == "10-Q" || form == "10-Q/A" || form == "10-K" || form == "10-K/A";
}

template <typename Setter>
void load_fact_category(
    const boost::json::object& gaap,
    const std::vector<std::string_view>& tags,
    const std::string_view unit_name,
    const std::unordered_map<std::string, Event>& events,
    std::unordered_map<std::string, FactCandidate>& candidates,
    Setter setter
) {
    for (std::size_t priority = 0; priority < tags.size(); ++priority) {
        const auto* tag_value = gaap.if_contains(tags[priority]);
        if (tag_value == nullptr || !tag_value->is_object()) continue;
        const auto& tag = tag_value->as_object();
        const auto* units_value = tag.if_contains("units");
        if (units_value == nullptr || !units_value->is_object()) continue;
        const auto& units = units_value->as_object();
        const auto* values_value = units.if_contains(unit_name);
        if (values_value == nullptr || !values_value->is_array()) continue;
        for (const auto& value : values_value->as_array()) {
            if (!value.is_object()) continue;
            const auto& item = value.as_object();
            const auto accession = json_string(item, "accn");
            const auto event = events.find(accession);
            const auto number = json_number(item, "val");
            if (event == events.end() || number == std::nullopt ||
                !supported_fundamental_form(event->second.form)) continue;
            const auto end = json_string(item, "end");
            if (end.empty()) continue;
            const auto candidate = FactCandidate{
                .value = *number, .end = end, .tag_priority = static_cast<int>(priority),
                .has_frame = !json_string(item, "frame").empty()
            };
            const auto existing = candidates.find(accession);
            const bool better = existing == candidates.end() ||
                candidate.tag_priority < existing->second.tag_priority ||
                (candidate.tag_priority == existing->second.tag_priority &&
                 candidate.has_frame && !existing->second.has_frame) ||
                (candidate.tag_priority == existing->second.tag_priority &&
                 candidate.has_frame == existing->second.has_frame && candidate.end > existing->second.end);
            if (better) {
                candidates[accession] = candidate;
                setter(accession, candidate.value);
            }
        }
    }
}

[[nodiscard]] std::string padded_cik(const std::string& cik) {
    const auto number = std::stoull(cik);
    std::ostringstream output;
    output << std::setw(10) << std::setfill('0') << number;
    return output.str();
}

[[nodiscard]] std::unordered_map<std::string, std::vector<RawSnapshot>> load_snapshots(
    const std::filesystem::path& companyfacts_dir,
    const std::vector<Holding>& holdings,
    const std::unordered_map<std::string, Event>& events,
    std::size_t& files_loaded,
    std::size_t& files_missing
) {
    std::unordered_map<std::string, std::vector<RawSnapshot>> snapshots;
    std::unordered_map<std::string, std::string> cik_by_symbol;
    for (const auto& holding : holdings) cik_by_symbol.emplace(holding.symbol, padded_cik(holding.cik));
    for (const auto& [symbol, cik] : cik_by_symbol) {
        const auto path = companyfacts_dir / ("CIK" + cik + ".json");
        if (!std::filesystem::exists(path)) {
            ++files_missing;
            continue;
        }
        ++files_loaded;
        std::ifstream input{path};
        std::ostringstream content;
        content << input.rdbuf();
        boost::system::error_code parse_error;
        const auto root = boost::json::parse(content.str(), parse_error);
        if (parse_error || !root.is_object()) throw std::runtime_error{"Invalid companyfacts JSON: " + path.string()};
        const auto* facts_value = root.as_object().if_contains("facts");
        if (facts_value == nullptr || !facts_value->is_object()) continue;
        const auto* gaap_value = facts_value->as_object().if_contains("us-gaap");
        if (gaap_value == nullptr || !gaap_value->is_object()) continue;
        std::unordered_map<std::string, RawSnapshot> by_accession;
        std::unordered_map<std::string, FactCandidate> revenue_candidates;
        std::unordered_map<std::string, FactCandidate> net_income_candidates;
        std::unordered_map<std::string, FactCandidate> assets_candidates;
        std::unordered_map<std::string, FactCandidate> liabilities_candidates;
        std::unordered_map<std::string, FactCandidate> eps_candidates;
        const auto ensure_snapshot = [&](const std::string& accession) -> RawSnapshot& {
            auto& snapshot = by_accession[accession];
            snapshot.accession = accession;
            snapshot.symbol = symbol;
            return snapshot;
        };
        const auto set_revenue = [&](const std::string& accession, const double value) {
            ensure_snapshot(accession).revenue = value;
        };
        const auto set_net_income = [&](const std::string& accession, const double value) {
            ensure_snapshot(accession).net_income = value;
        };
        const auto set_assets = [&](const std::string& accession, const double value) {
            ensure_snapshot(accession).assets = value;
        };
        const auto set_liabilities = [&](const std::string& accession, const double value) {
            ensure_snapshot(accession).liabilities = value;
        };
        const auto set_eps = [&](const std::string& accession, const double value) {
            ensure_snapshot(accession).eps = value;
        };
        const auto& gaap = gaap_value->as_object();
        load_fact_category(gaap, {"Revenues", "RevenueFromContractWithCustomerExcludingAssessedTax", "SalesRevenueNet"}, "USD", events, revenue_candidates, set_revenue);
        load_fact_category(gaap, {"NetIncomeLoss", "ProfitLoss"}, "USD", events, net_income_candidates, set_net_income);
        load_fact_category(gaap, {"Assets"}, "USD", events, assets_candidates, set_assets);
        load_fact_category(gaap, {"Liabilities"}, "USD", events, liabilities_candidates, set_liabilities);
        load_fact_category(gaap, {"EarningsPerShareDiluted"}, "USD/shares", events, eps_candidates, set_eps);
        for (auto& [accession, snapshot] : by_accession) {
            const auto event = events.find(accession);
            if (event == events.end()) continue;
            snapshot.accepted_at = event->second.accepted_at;
            snapshot.trading_date = event->second.trading_date;
            snapshot.sector = event->second.sector;
            if (snapshot.revenue || snapshot.net_income || snapshot.assets || snapshot.liabilities || snapshot.eps) {
                snapshots[symbol].push_back(std::move(snapshot));
            }
        }
    }
    for (auto& [unused_symbol, items] : snapshots) {
        static_cast<void>(unused_symbol);
        std::ranges::sort(items, [](const auto& left, const auto& right) {
            return std::tie(left.accepted_at, left.accession) < std::tie(right.accepted_at, right.accession);
        });
    }
    return snapshots;
}

[[nodiscard]] std::optional<Snapshot> derive_snapshot(
    const RawSnapshot& current,
    const RawSnapshot* previous
) {
    Snapshot result{.trading_date = current.trading_date, .symbol = current.symbol};
    if (current.revenue && previous != nullptr && previous->revenue && *current.revenue > 0.0 && *previous->revenue > 0.0) {
        result.revenue_growth = clipped_change(*current.revenue, *previous->revenue);
        result.has_revenue = true;
    }
    if (current.net_income && current.revenue && *current.revenue != 0.0) {
        result.net_margin = std::clamp(*current.net_income / *current.revenue, -5.0, 5.0);
        result.has_net_margin = true;
    }
    if (current.assets && previous != nullptr && previous->assets && *current.assets > 0.0 && *previous->assets > 0.0) {
        result.assets_growth = clipped_change(*current.assets, *previous->assets);
        result.has_assets_growth = true;
    }
    if (current.liabilities && previous != nullptr && previous->liabilities && *current.liabilities > 0.0 && *previous->liabilities > 0.0) {
        result.liabilities_growth = clipped_change(*current.liabilities, *previous->liabilities);
        result.has_liabilities_growth = true;
    }
    if (current.eps && previous != nullptr && previous->eps && std::isfinite(*current.eps) && std::isfinite(*previous->eps)) {
        result.eps_change = std::clamp(*current.eps - *previous->eps, -10.0, 10.0);
        result.has_eps_change = true;
    }
    return result;
}

[[nodiscard]] std::unordered_map<std::string, std::vector<Snapshot>> derive_all_snapshots(
    const std::unordered_map<std::string, std::vector<RawSnapshot>>& raw
) {
    std::unordered_map<std::string, std::vector<Snapshot>> result;
    for (const auto& [symbol, items] : raw) {
        std::vector<Snapshot> derived;
        for (std::size_t index = 0; index < items.size(); ++index) {
            const auto* previous = index == 0 ? nullptr : &items[index - 1];
            const auto snapshot = derive_snapshot(items[index], previous);
            if (snapshot) derived.push_back(*snapshot);
        }
        result.emplace(symbol, std::move(derived));
    }
    return result;
}

[[nodiscard]] std::vector<double> aggregate_for(
    const std::string_view date,
    const std::string_view sector,
    const std::vector<Holding>& holdings,
    const std::unordered_map<std::string, std::vector<Snapshot>>& snapshots
) {
    Aggregate aggregate;
    for (const auto& holding : holdings) {
        if (holding.sector != sector) continue;
        const auto found = snapshots.find(holding.symbol);
        if (found == snapshots.end()) continue;
        const auto& items = found->second;
        const auto latest = std::ranges::upper_bound(items, date, {}, &Snapshot::trading_date);
        if (latest == items.begin()) continue;
        const auto& item = *std::prev(latest);
        ++aggregate.coverage;
        if (item.trading_date == date) ++aggregate.filing_count;
        if (item.has_revenue) { aggregate.revenue_growth_sum += item.revenue_growth; ++aggregate.revenue_growth_count; }
        if (item.has_net_margin) { aggregate.net_margin_sum += item.net_margin; ++aggregate.net_margin_count; }
        if (item.has_assets_growth) { aggregate.assets_growth_sum += item.assets_growth; ++aggregate.assets_growth_count; }
        if (item.has_liabilities_growth) { aggregate.liabilities_growth_sum += item.liabilities_growth; ++aggregate.liabilities_growth_count; }
        if (item.has_eps_change) { aggregate.eps_change_sum += item.eps_change; ++aggregate.eps_change_count; }
    }
    const auto mean = [](const double sum, const std::size_t count) {
        return count == 0 ? 0.0 : sum / static_cast<double>(count);
    };
    return {
        static_cast<double>(aggregate.filing_count), static_cast<double>(aggregate.coverage),
        mean(aggregate.revenue_growth_sum, aggregate.revenue_growth_count),
        mean(aggregate.net_margin_sum, aggregate.net_margin_count),
        mean(aggregate.assets_growth_sum, aggregate.assets_growth_count),
        mean(aggregate.liabilities_growth_sum, aggregate.liabilities_growth_count),
        mean(aggregate.eps_change_sum, aggregate.eps_change_count)
    };
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 7) {
            std::cout << "Usage: arrakis-build-pooled-sector-sec-fundamentals-control <companyfacts_dir> "
                         "<holdings_cik.csv> <sec_events.csv> <pooled_market.csv> <output.csv> <to-date>\n";
            return 0;
        }
        const auto holdings = load_holdings(argv[2]);
        std::unordered_map<std::string, Event> events_by_accession;
        const auto events = load_events(argv[3], events_by_accession);
        std::size_t files_loaded = 0;
        std::size_t files_missing = 0;
        const auto raw_snapshots = load_snapshots(argv[1], holdings, events_by_accession, files_loaded, files_missing);
        const auto snapshots = derive_all_snapshots(raw_snapshots);

        std::ifstream market{argv[4]};
        if (!market) throw std::runtime_error{"Could not open pooled market dataset"};
        const auto market_header = read_record(market);
        const auto market_date = column_index(market_header, "date");
        const auto market_target = column_index(market_header, "target_next_close_up");
        std::ofstream output{argv[5]};
        if (!output) throw std::runtime_error{"Could not write fundamentals control dataset"};
        output << std::setprecision(17)
               << "date,fundamental_filing_count,fundamental_issuer_coverage,fundamental_revenue_growth_mean,fundamental_net_margin_mean,fundamental_assets_growth_mean,fundamental_liabilities_growth_mean,fundamental_eps_change_mean";
        for (std::size_t index = 0; index < market_header.size(); ++index) {
            if (index != market_date && index != market_target) output << ',' << market_header[index];
        }
        output << ',' << market_header[market_target] << '\n';

        std::size_t rows_written = 0;
        while (true) {
            const auto row = read_record(market);
            if (row.empty()) break;
            if (row.size() != market_header.size()) throw std::runtime_error{"Malformed pooled market row"};
            const auto separator = row[market_date].find('|');
            if (separator == std::string::npos) throw std::runtime_error{"Pooled market key lacks sector"};
            const auto date = row[market_date].substr(0, separator);
            if (date > argv[6]) continue;
            const auto sector = row[market_date].substr(separator + 1);
            const auto values = aggregate_for(date, sector, holdings, snapshots);
            output << csv_escape(row[market_date]);
            for (const auto value : values) output << ',' << value;
            for (std::size_t index = 0; index < market_header.size(); ++index) {
                if (index != market_date && index != market_target) output << ',' << row[index];
            }
            output << ',' << row[market_target] << '\n';
            ++rows_written;
        }

        std::ofstream manifest{std::string{argv[5]} + ".manifest.json"};
        if (!manifest) throw std::runtime_error{"Could not write fundamentals manifest"};
        manifest << "{\n"
                 << "  \"companyfacts_dir\": \"" << argv[1] << "\",\n"
                 << "  \"holdings\": \"" << argv[2] << "\",\n"
                 << "  \"sec_events\": \"" << argv[3] << "\",\n"
                 << "  \"market_dataset\": \"" << argv[4] << "\",\n"
                 << "  \"rows_written\": " << rows_written << ",\n"
                 << "  \"event_rows\": " << events.size() << ",\n"
                 << "  \"companyfacts_files_loaded\": " << files_loaded << ",\n"
                 << "  \"companyfacts_files_missing\": " << files_missing << ",\n"
                 << "  \"features\": [\"fundamental_filing_count\",\"fundamental_issuer_coverage\",\"fundamental_revenue_growth_mean\",\"fundamental_net_margin_mean\",\"fundamental_assets_growth_mean\",\"fundamental_liabilities_growth_mean\",\"fundamental_eps_change_mean\"],\n"
                 << "  \"availability_policy\": \"SEC companyfacts facts are joined only through matching accepted 10-Q/10-K accessions; snapshots are used on assigned sessions and later\",\n"
                 << "  \"target_preserved\": \"target_next_close_up copied unchanged from the corrected close[t+1] > close[t] panel\"\n}\n";
        std::cout << "Wrote " << rows_written << " pooled SEC-fundamentals rows; loaded "
                  << files_loaded << " companyfacts files\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-build-pooled-sector-sec-fundamentals-control: " << error.what() << '\n';
        return 1;
    }
}
