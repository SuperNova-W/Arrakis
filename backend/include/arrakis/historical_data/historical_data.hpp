#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace arrakis::historical_data {

struct MarketBar final {
    std::string symbol;
    std::int64_t timestamp_utc{0};
    double open{0.0};
    double high{0.0};
    double low{0.0};
    double close{0.0};
    double volume{0.0};
};

struct CandleResponse final {
    std::string status;
    std::vector<std::int64_t> timestamps;
    std::vector<double> opens;
    std::vector<double> highs;
    std::vector<double> lows;
    std::vector<double> closes;
    std::vector<double> volumes;
};

struct FinnhubClientConfig final {
    std::string base_url{"https://finnhub.io/api/v1"};
    std::string api_key{};
    std::string resolution{"5"};
    int chunk_months{1};
    double max_requests_per_second{1.0};
    int max_retry_attempts{6};
    int initial_retry_delay_ms{1000};
    int maximum_retry_delay_ms{60000};
    int request_timeout_seconds{30};
};

struct RequestWindow final {
    std::chrono::system_clock::time_point start;
    std::chrono::system_clock::time_point end;
};

struct ChunkPlan final {
    std::vector<RequestWindow> windows;
};

struct ChunkManifest final {
    std::string provider{"finnhub"};
    std::string symbol;
    std::string resolution;
    std::string requested_start_utc;
    std::string requested_end_utc;
    std::string actual_first_timestamp_utc;
    std::string actual_last_timestamp_utc;
    std::size_t row_count{0};
    std::size_t duplicate_count{0};
    std::size_t invalid_row_count{0};
    std::string response_status{"unknown"};
    std::string downloaded_at_utc;
    std::string checksum;
    bool complete{false};
};

struct SymbolSummary final {
    std::string symbol;
    std::string requested_start;
    std::string requested_end;
    std::string actual_start;
    std::string actual_end;
    std::size_t total_rows{0};
    std::size_t completed_chunks{0};
    std::size_t no_data_chunks{0};
    std::size_t failed_chunks{0};
    std::size_t missing_bar_count{0};
    std::string coverage_status{"unknown"};
    std::string adjustment_status{"unverified"};
};

class FinnhubClient final {
  public:
    explicit FinnhubClient(FinnhubClientConfig config);

    [[nodiscard]] CandleResponse get_candles(
        std::string_view symbol,
        std::string_view resolution,
        std::chrono::system_clock::time_point from,
        std::chrono::system_clock::time_point to
    );

  private:
    FinnhubClientConfig config_;
};

[[nodiscard]] std::string load_api_key_from_env_file(std::string_view directory = {});
[[nodiscard]] std::chrono::system_clock::time_point parse_datetime(std::string_view value);
[[nodiscard]] std::string format_datetime(std::chrono::system_clock::time_point value);
[[nodiscard]] std::string format_datetime_utc(std::chrono::system_clock::time_point value);
[[nodiscard]] std::chrono::seconds to_epoch_seconds(std::chrono::system_clock::time_point value);
[[nodiscard]] std::chrono::system_clock::time_point from_epoch_seconds(std::int64_t value);

[[nodiscard]] ChunkPlan build_chunk_plan(
    std::chrono::system_clock::time_point start,
    std::chrono::system_clock::time_point end,
    int chunk_months
);

[[nodiscard]] std::vector<MarketBar> validate_and_deduplicate(
    const std::vector<MarketBar>& bars,
    std::size_t& duplicate_count,
    std::size_t& invalid_row_count
);

[[nodiscard]] std::string sha256_hex(std::string_view data);
[[nodiscard]] std::string write_csv_chunk(
    const std::string& symbol,
    const std::vector<MarketBar>& bars,
    const std::filesystem::path& output_dir
);

[[nodiscard]] ChunkManifest write_manifest(
    const std::string& symbol,
    const std::string& resolution,
    const RequestWindow& window,
    const std::vector<MarketBar>& bars,
    const std::string& response_status,
    const std::filesystem::path& output_dir
);

[[nodiscard]] std::string serialize_manifest(const ChunkManifest& manifest);
[[nodiscard]] ChunkManifest parse_manifest(std::string_view payload);

}  // namespace arrakis::historical_data
