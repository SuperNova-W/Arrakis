# Arrakis C++ backend

The backend contains a C++20 streaming vertical slice:

- An XGBoost baseline trainer for time-ordered feature CSVs.
- A TLS-secured Boost.Beast WebSocket client for Finnhub's real-time stock trade feed.
- A protobuf/librdkafka path from `market.raw.trades` to `market.bars.1m`.
- An event-time, per-symbol one-minute aggregator with bounded deduplication and watermarks.

The deployable services are isolated under `services/`: `services/bar_aggregator` owns the bar
aggregator executable and its service-level CMake target. Kafka, protobuf, configuration, metrics,
and serialization remain shared libraries so other consumers can be added without coupling service
entry points together.

The ML implementation is isolated under `services/ml_model/`. It owns the dataset, metrics, sector
feature/XGBoost code, trainer executable, and sample training data. Its reusable library remains
named `arrakis_model_core`, and the trainer remains `arrakis-train-xgboost` for command-line
compatibility.

Shared market code is isolated under `libs/market/`, shared protobuf conversion under
`libs/serialization/`, and bar state/aggregation under
`services/bar_aggregator/`. The bar service depends on the shared libraries through explicit CMake
targets; it no longer owns or compiles market normalization or serialization implementation files.

The feature-engineering service is isolated under `services/feature_engine/`. It consumes completed
`market.bars.5m` protobuf bars, aligns all configured sector/context ETFs by `bar_end`, maintains a
bounded history, and publishes deterministic `sector-features-v1` events to `market.features`.
It requires a complete 18-symbol timestamp bucket; expired buckets are reported to
`market.feature.errors` without forward-filling. Feature calculations use log returns, sample
standard deviation, current-bar-inclusive SMAs, and current-bar-excluded relative-volume averages.
The initial service uses in-memory state plus deterministic Kafka replay after restart and does not
claim exactly-once processing.

The XLK prediction service is isolated under `services/prediction_service/`. It consumes named,
versioned `FeatureEvent` protobufs from `market.features`, validates them against the loaded
artifact metadata/schema, runs the existing XGBoost C API model, and publishes `PredictionEvent`
protobufs to `model.predictions`. It commits only after output is enqueued; malformed or incompatible
events go to `dead-letter.events`. The configured artifact directory must contain an XLK model,
metadata, and feature schema. Legacy artifacts with incompatible feature ordering are rejected.

The stream client authenticates without logging credentials, subscribes to one or more symbols,
parses batched Finnhub messages, and publishes normalized protobuf trade events to Kafka. The
The streaming services use at-least-once Kafka processing. Deterministic trade/bar IDs and duplicate
suppression make replay safe within the in-memory process; this is not exactly-once.

This is training infrastructure, not evidence of predictive market edge. The included CSV is
synthetic smoke-test data and must never be presented as a research result.

## Input contract

The trainer expects:

```text
date,<numeric feature columns>,target_up_5d
```

- `date` contains unique, strictly increasing ISO-8601 dates.
- Feature columns are numeric. Empty cells and `nan` are passed to XGBoost as missing values.
- The target is binary: `0` or `1`.
- Rows are never shuffled. The most recent fraction becomes validation data.

## Dependencies

- C++20 compiler
- CMake 3.18 or newer (3.23+ for the included presets)
- Ninja when using the presets
- XGBoost with its CMake package installed
- Boost (Beast, Asio, and JSON)
- OpenSSL

XGBoost's stable C API is used intentionally. Its internal C++ API is not a stable public boundary.
On macOS, XGBoost's official build documentation also requires OpenMP support (`libomp`).

The required native packages can be installed with Homebrew:

```bash
brew install cmake ninja xgboost boost openssl@3
```

## Build

When XGBoost is installed in a standard CMake prefix:

```bash
cd backend
cmake --preset release
cmake --build --preset release
```

If XGBoost is installed elsewhere, add its install prefix:

```bash
cmake --preset release -DCMAKE_PREFIX_PATH=/path/to/xgboost/install
cmake --build --preset release
```

The CSV/splitting/metrics core can be built without XGBoost:

```bash
cmake --preset core-only
cmake --build --preset core-only
ctest --preset core-only
```

Run all backend tests after a normal build:

```bash
ctest --test-dir build/release --output-on-failure
```

## Stream live Finnhub stock data

Create a Finnhub API token in your Finnhub account and export it in your local shell. Do not
put real secrets in source files, command-line arguments, screenshots, or commits.

```bash
export FINNHUB_API_KEY="your-api-token"
```

You can also keep a local ignored env file:

```bash
cp .env.example .env
# edit .env with your real Finnhub token
set -a
source .env
set +a
```

Build and stream trades from Finnhub:

```bash
./build/release/arrakis-market-stream \
  --symbols IWM,SPY \
  --max-events 20
```

`--symbols` defaults to the configured ETF universe. `--max-events 0` (the default) keeps the stream open until the
process is interrupted. Status messages are written to stderr; each market event is written to
stdout as one normalized JSON object:

```json
{"event_type":"trade","source":"finnhub","symbol":"IWM","price":126.55,"size":3,"timestamp_ms":1721577104208,"conditions":["@"]}
```

Finnhub's WebSocket stock stream provides last-price trades and volume, not bid/ask quote updates.
The stream can contain multiple trades in one frame. Finnhub documents a one-connection limit per
API key, and activity may be sparse outside regular and extended market sessions.

## Kafka slice

Set `KAFKA_BOOTSTRAP_SERVERS` (defaults to `localhost:9092`) and start local infrastructure with:

```bash
docker compose up kafka kafka-init kafka-ui prometheus grafana
./build/release/arrakis-market-stream
./build/release/bar-aggregator
```

Trades and bars are protobuf messages keyed by symbol. Empty intervals emit no synthetic bar. The
watermark is maintained independently per symbol and is that symbol's maximum event time minus the
configured allowed lateness. Malformed input is published as `dead-letter.events`; trades arriving
after finalization are published as `market.late.trades`. State is in memory and is rebuilt by Kafka
replay after restart; a changelog or snapshot store is a future durability step.

Both services expose Prometheus text metrics on the configured `metrics_port` (9101 for ingestion,
9102 for aggregation). Kafka delivery callbacks log asynchronous delivery failures. The consumer
commits only after deserialization, state update, output enqueue, and late/dead-letter routing. This
is at-least-once processing, not end-to-end exactly-once processing.

## Run the sample

```bash
./build/release/arrakis-train-xgboost \
  --input services/ml_model/data/sample_features.csv \
  --target target_up_5d \
  --model-output artifacts/xgboost_baseline.json \
  --rounds 75
```

The trainer writes:

- `artifacts/xgboost_baseline.json`: reusable XGBoost model.
- `artifacts/xgboost_baseline.json.metrics.json`: validation boundaries and baseline metrics.

## Why this split is only a starting point

A single chronological holdout is safer than a random split, but it is not the final evaluation.
The next modeling milestone should replace it with expanding-window walk-forward folds and compare
XGBoost against a simple Logistic Regression baseline and non-ML market benchmarks.
