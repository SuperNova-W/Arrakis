#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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

[[nodiscard]] std::string json_string(const boost::json::array& array, const std::size_t index) {
    if (index >= array.size() || !array[index].is_string()) return {};
    return std::string{array[index].as_string()};
}

struct Aggregate final {
    std::size_t form4_count{};
    std::size_t form3_5_count{};
    std::size_t schedule13_count{};
    std::unordered_map<std::string, bool> issuers;
};

[[nodiscard]] std::unordered_map<std::string, std::string> load_symbol_sectors(
    const std::filesystem::path& path,
    std::unordered_map<std::string, std::string>& cik_sectors
) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error{"Could not open holdings: " + path.string()};
    std::unordered_map<std::string, std::string> cik_to_sector;
    while (true) {
        const auto row = read_record(input);
        if (row.empty()) break;
        if (row.size() < 4 || row[0] == "sector" || row[3].empty()) continue;
        cik_to_sector[row[3]] = row[0];
        cik_sectors[row[3]] = row[0];
    }
    if (cik_to_sector.empty()) throw std::runtime_error{"Holdings contain no CIKs"};
    return cik_to_sector;
}

[[nodiscard]] bool is_form4(const std::string_view form) {
    return form == "4" || form == "4/A";
}

[[nodiscard]] bool is_form3_5(const std::string_view form) {
    return form == "3" || form == "3/A" || form == "5" || form == "5/A";
}

[[nodiscard]] bool is_schedule13(const std::string_view form) {
    return form.starts_with("SCHEDULE 13") || form.starts_with("SC 13");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 6) {
            std::cout << "Usage: arrakis-build-pooled-sector-sec-insider-control <submissions_dir> "
                         "<holdings_cik.csv> <pooled_market.csv> <output.csv> <to-date>\n";
            return 0;
        }
        std::unordered_map<std::string, std::string> cik_sectors;
        const auto unused = load_symbol_sectors(argv[2], cik_sectors);
        static_cast<void>(unused);

        std::ifstream market{argv[3]};
        if (!market) throw std::runtime_error{"Could not open pooled market dataset"};
        const auto market_header = read_record(market);
        const auto market_date = column_index(market_header, "date");
        const auto market_target = column_index(market_header, "target_next_close_up");
        std::vector<std::string> sessions;
        while (true) {
            const auto row = read_record(market);
            if (row.empty()) break;
            if (row.size() != market_header.size()) throw std::runtime_error{"Malformed pooled market row"};
            const auto separator = row[market_date].find('|');
            if (separator == std::string::npos) throw std::runtime_error{"Pooled market key lacks sector"};
            const auto date = row[market_date].substr(0, separator);
            if (sessions.empty() || sessions.back() != date) sessions.push_back(date);
        }
        if (sessions.empty()) throw std::runtime_error{"Pooled market has no sessions"};

        std::unordered_map<std::string, Aggregate> aggregates;
        std::size_t issuer_files = 0;
        for (const auto& [cik, sector] : cik_sectors) {
            std::ostringstream filename;
            filename << "CIK" << std::setw(10) << std::setfill('0') << std::stoull(cik) << ".json";
            const auto path = std::filesystem::path{argv[1]} / filename.str();
            if (!std::filesystem::exists(path)) continue;
            ++issuer_files;
            std::ifstream input{path};
            std::ostringstream content;
            content << input.rdbuf();
            boost::system::error_code parse_error;
            const auto root = boost::json::parse(content.str(), parse_error);
            if (parse_error || !root.is_object()) throw std::runtime_error{"Invalid submissions JSON: " + path.string()};
            const auto* filings_value = root.as_object().if_contains("filings");
            if (filings_value == nullptr || !filings_value->is_object()) continue;
            const auto* recent_value = filings_value->as_object().if_contains("recent");
            if (recent_value == nullptr || !recent_value->is_object()) continue;
            const auto& recent = recent_value->as_object();
            const auto* forms_value = recent.if_contains("form");
            const auto* accepted_value = recent.if_contains("acceptanceDateTime");
            const auto* accession_value = recent.if_contains("accessionNumber");
            if (forms_value == nullptr || accepted_value == nullptr || accession_value == nullptr ||
                !forms_value->is_array() || !accepted_value->is_array() || !accession_value->is_array()) continue;
            const auto& forms = forms_value->as_array();
            const auto& accepted = accepted_value->as_array();
            const auto& accessions = accession_value->as_array();
            for (std::size_t index = 0; index < forms.size(); ++index) {
                const auto form = json_string(forms, index);
                if (!is_form4(form) && !is_form3_5(form) && !is_schedule13(form)) continue;
                const auto accepted_at = json_string(accepted, index);
                if (accepted_at.size() < 10) continue;
                const auto session = std::ranges::upper_bound(sessions, accepted_at.substr(0, 10));
                if (session == sessions.end() || *session > argv[5]) continue;
                auto& aggregate = aggregates[*session + "|" + sector];
                aggregate.issuers[cik] = true;
                if (is_form4(form)) ++aggregate.form4_count;
                if (is_form3_5(form)) ++aggregate.form3_5_count;
                if (is_schedule13(form)) ++aggregate.schedule13_count;
                static_cast<void>(json_string(accessions, index));
            }
        }

        market.clear();
        market.seekg(0);
        const auto header_again = read_record(market);
        if (header_again != market_header) throw std::runtime_error{"Pooled market header changed while reading"};
        std::ofstream output{argv[4]};
        if (!output) throw std::runtime_error{"Could not write insider control dataset"};
        output << "date,insider_form4_count,insider_form3_5_count,insider_schedule13_count,insider_issuer_breadth";
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
            if (row[market_date].substr(0, separator) > argv[5]) continue;
            const auto found = aggregates.find(row[market_date]);
            const Aggregate empty{};
            const auto& item = found == aggregates.end() ? empty : found->second;
            output << csv_escape(row[market_date]) << ',' << item.form4_count << ',' << item.form3_5_count << ','
                   << item.schedule13_count << ',' << item.issuers.size();
            for (std::size_t index = 0; index < market_header.size(); ++index) {
                if (index != market_date && index != market_target) output << ',' << row[index];
            }
            output << ',' << row[market_target] << '\n';
            ++rows_written;
        }
        std::ofstream manifest{std::string{argv[4]} + ".manifest.json"};
        if (!manifest) throw std::runtime_error{"Could not write insider manifest"};
        manifest << "{\n"
                 << "  \"submissions_dir\": \"" << argv[1] << "\",\n"
                 << "  \"holdings\": \"" << argv[2] << "\",\n"
                 << "  \"market_dataset\": \"" << argv[3] << "\",\n"
                 << "  \"rows_written\": " << rows_written << ",\n"
                 << "  \"issuer_files_loaded\": " << issuer_files << ",\n"
                 << "  \"features\": [\"insider_form4_count\",\"insider_form3_5_count\",\"insider_schedule13_count\",\"insider_issuer_breadth\"],\n"
                 << "  \"availability_policy\": \"SEC acceptanceDateTime assigned to first market session strictly after acceptance calendar date\",\n"
                 << "  \"target_preserved\": \"target_next_close_up copied unchanged from the corrected close-direction panel\"\n}\n";
        std::cout << "Wrote " << rows_written << " pooled SEC-insider rows from "
                  << issuer_files << " issuer submissions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arrakis-build-pooled-sector-sec-insider-control: " << error.what() << '\n';
        return 1;
    }
}
