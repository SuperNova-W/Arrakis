#include "arrakis/historical_data/historical_data.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void loads_api_key_from_dotenv_file() {
    const auto temp_dir = std::filesystem::temp_directory_path() / "arrakis_dotenv_test";
    std::filesystem::create_directories(temp_dir);
    std::ofstream dotenv{temp_dir / ".env"};
    dotenv << "FINNHUB_API_KEY=from-dotenv\n";
    dotenv.close();

    const auto key = arrakis::historical_data::load_api_key_from_env_file(temp_dir.string());
    if (key != "from-dotenv") {
        throw std::runtime_error{"expected .env API key to be loaded"};
    }

    std::filesystem::remove_all(temp_dir);
}

}  // namespace

int main() {
    using namespace arrakis::historical_data;

    loads_api_key_from_dotenv_file();

    const auto start = parse_datetime("2024-01-01T00:00:00Z");
    const auto end = parse_datetime("2024-03-01T00:00:00Z");
    const auto plan = build_chunk_plan(start, end, 1);
    if (plan.windows.size() != 2) {
        std::cerr << "expected two chunks, got " << plan.windows.size() << '\n';
        return 1;
    }

    std::vector<MarketBar> bars;
    bars.push_back(MarketBar{"XLK", 1704067200, 100.0, 101.0, 99.5, 100.5, 1000.0});
    bars.push_back(MarketBar{"XLK", 1704153600, 100.5, 102.0, 100.0, 101.5, 1100.0});
    bars.push_back(MarketBar{"XLK", 1704153600, 100.5, 102.0, 100.0, 101.5, 1100.0});
    bars.push_back(MarketBar{"XLK", 1704240000, 101.5, 102.5, 101.0, 102.0, 1200.0});

    std::size_t duplicates = 0;
    std::size_t invalid = 0;
    const auto deduped = validate_and_deduplicate(bars, duplicates, invalid);
    if (deduped.size() != 3 || duplicates != 1 || invalid != 0) {
        std::cerr << "unexpected validation results" << '\n';
        return 1;
    }

    const auto temp_dir = std::filesystem::temp_directory_path() / "arrakis_historical_test";
    std::filesystem::remove_all(temp_dir);
    const auto csv_path = write_csv_chunk("XLK", deduped, temp_dir);
    if (!std::filesystem::exists(csv_path)) {
        std::cerr << "CSV chunk was not written" << '\n';
        return 1;
    }

    const auto manifest = write_manifest("XLK", "D", plan.windows.front(), deduped, "ok", temp_dir);
    if (manifest.row_count != deduped.size()) {
        std::cerr << "manifest row count mismatch" << '\n';
        return 1;
    }

    const auto serialized = serialize_manifest(manifest);
    const auto parsed = parse_manifest(serialized);
    if (parsed.symbol != "XLK" || parsed.row_count != deduped.size()) {
        std::cerr << "manifest roundtrip failed" << '\n';
        return 1;
    }

    std::filesystem::remove_all(temp_dir);
    return 0;
}
