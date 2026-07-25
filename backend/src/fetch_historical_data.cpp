#include "arrakis/historical_data/historical_data.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
    try {
        std::string symbol = "XLK";
        std::string resolution = "D";
        std::string output_dir = "./data/history";
        std::string start = "2024-01-01T00:00:00Z";
        std::string end = "2024-03-01T00:00:00Z";

        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--symbol" && index + 1 < argc) {
                symbol = argv[++index];
            } else if (argument == "--resolution" && index + 1 < argc) {
                resolution = argv[++index];
            } else if (argument == "--output" && index + 1 < argc) {
                output_dir = argv[++index];
            } else if (argument == "--start" && index + 1 < argc) {
                start = argv[++index];
            } else if (argument == "--end" && index + 1 < argc) {
                end = argv[++index];
            } else {
                throw std::runtime_error("Unknown argument: " + std::string(argument));
            }
        }

        using namespace arrakis::historical_data;
        FinnhubClientConfig config;
        config.resolution = resolution;
        config.api_key = std::getenv("FINNHUB_API_KEY") ? std::getenv("FINNHUB_API_KEY") : "";
        FinnhubClient client{config};

        const auto from = parse_datetime(start);
        const auto to = parse_datetime(end);
        const auto candles = client.get_candles(symbol, resolution, from, to);

        std::vector<MarketBar> bars;
        bars.reserve(candles.timestamps.size());
        for (std::size_t index = 0; index < candles.timestamps.size(); ++index) {
            if (index >= candles.opens.size() || index >= candles.highs.size() || index >= candles.lows.size() ||
                index >= candles.closes.size() || index >= candles.volumes.size()) {
                continue;
            }
            MarketBar bar;
            bar.symbol = symbol;
            bar.timestamp_utc = candles.timestamps.at(index);
            bar.open = candles.opens.at(index);
            bar.high = candles.highs.at(index);
            bar.low = candles.lows.at(index);
            bar.close = candles.closes.at(index);
            bar.volume = candles.volumes.at(index);
            bars.push_back(bar);
        }

        std::size_t duplicates = 0;
        std::size_t invalid_rows = 0;
        const auto validated = validate_and_deduplicate(bars, duplicates, invalid_rows);
        const auto output_path = std::filesystem::path{output_dir};
        const auto csv_path = write_csv_chunk(symbol, validated, output_path);
        const auto window = RequestWindow{from, to};
        const auto manifest = write_manifest(symbol, resolution, window, validated, candles.status, output_path);

        std::cout << "Wrote " << validated.size() << " bars to " << csv_path << '\n';
        std::cout << "Manifest: " << manifest.symbol << " rows=" << manifest.row_count << " status=" << manifest.response_status << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "fetch-historical-data: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
