# Arrakis market-data platform

This backend implements the market-data-first vertical slice:

```text
Finnhub WebSocket → market-ingestion → Kafka: market.raw.trades
→ market-api in-memory OHLCV → REST/WebSocket → frontend

Approved news source → news-ingestion → Kafka: news.raw.articles
→ news-enricher → frozen FinBERT ONNX → daily XLK features
→ one XGBoost model → market-api → frontend insights
```

Kafka provides raw-trade buffering and replay. The frontend market path uses a dedicated consumer
group and builds bounded, duplicate-safe 1-minute and 5-minute bars in memory. PostgreSQL is not a
market-price dependency; it is reserved for the news-feature and ML inference path. The separate
bar-aggregator can still persist research datasets. The platform makes no exactly-once claim.

The initial ML path is XLK-only and close-to-next-close. FinBERT is frozen and runs through the
ONNX Runtime C++ wrapper. The single XGBoost artifact consumes a fixed market-plus-news vector.
The API fails closed when either versioned artifact is missing or the feature schema does not match.

## Services

- `market-ingestion`: TLS Finnhub WebSocket client and protobuf Kafka producer.
- `bar-aggregator`: event-time 1-minute aggregation, 5-minute derivation, and PostgreSQL writer.
- `market-api`: Boost.Beast REST/WebSocket gateway with Kafka-backed in-memory market bars;
  PostgreSQL is used only by its ML/news routes.
- `news-ingestion`: approved-source/fixture JSONL normalizer and protobuf Kafka producer; the default Compose command polls Finnhub company news for XLK.
- `news-enricher`: point-in-time XLK filter, FinBERT ONNX inference, PostgreSQL writer, and daily aggregator.

The older `services/feature_engine/` and `services/prediction_service/` pair is
an archived, market-bars-only benchmark slice retained for a future replay or
price-only research phase. It is intentionally not wired into Docker or the
frontend; the active recommendation path is the FinBERT-plus-XGBoost pipeline.
See [`docs/legacy-ml-path.md`](docs/legacy-ml-path.md).
- `market-ui`: React/Vite frontend in `../frontend`.

The existing feature-engine and prediction-service sources remain in the repository for a future ML
phase but are excluded from the v1 Docker Compose runtime.

## Local development

Install native dependencies:

```bash
brew install cmake ninja boost openssl@3 protobuf librdkafka libpq onnxruntime
```

Build and test:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Run the frontend checks:

```bash
cd ../frontend
npm ci
npm run build
npm run lint
```

For deterministic local Kafka/integration tests, replay the checked-in Finnhub JSONL fixture
through the same normalization and protobuf producer path used by live mode:

```bash
KAFKA_BOOTSTRAP_SERVERS=localhost:9092 \
  ./build/debug/market-ingestion \
  --fixture tests/fixtures/finnhub_spy_trades.jsonl
```

Fixture mode does not require `FINNHUB_API_KEY`; live mode subscribes to the configured ETF
universe and does require it.

## Docker Compose

Copy the environment file and set credentials:

```bash
cp .env.example .env
# set FINNHUB_API_KEY for the live Finnhub stream
# replace the local-only POSTGRES_PASSWORD before using any shared environment
# fetch the exact FinBERT files before building; see models/finbert/README.md
docker compose up --build
```

`docker compose config` and the database/API fallback can start without a private credential file:
Compose uses the explicitly labeled local-development database password from `.env.example` as its
fallback. It is not suitable for a shared or deployed environment. A missing `FINNHUB_API_KEY`
causes only the live ingestion process to remain unavailable; the API can still serve persisted
bars. Set the key in the ignored `.env` file to receive current trades.

The API image includes the XGBoost inference runtime even though the model trainer is disabled in
the container build. The news recommendation routes remain unavailable, without stopping market
REST/WebSocket routes, until a verified `artifacts/xlk_news_xgboost.json` artifact is present. The
repository's older baseline artifacts are not substituted because they do not satisfy the approved
FinBERT-plus-XGBoost feature contract.

Local endpoints:

```text
UI:         http://localhost:3000
REST API:   http://localhost:8080
Kafka UI:   http://localhost:8081
Prometheus: http://localhost:9090
Grafana:    http://localhost:3001
PostgreSQL: localhost:5432
```

Set `MARKET_API_HOST_PORT` if port 8080 is already in use. If the browser UI origin changes, set
`CORS_ALLOWED_ORIGINS` to that exact origin (for example `http://127.0.0.1:5173`) before starting
the API.

The default news services are continuous: `news-ingestion` polls Finnhub every 15 minutes with a
three-day lookback, and `news-enricher` derives the current US trading date when the fixed replay
window variables are unset. Override `NEWS_POLL_INTERVAL_SECONDS` and
`NEWS_POLL_LOOKBACK_DAYS` in `.env` for another cadence.

The local migration container applies the SQL files in `migrations/` to PostgreSQL. In production,
point `SUPABASE_DB_URL` at the Supabase SSL connection string and apply the same migrations through
the Supabase migration workflow.

## API

```text
GET /health
GET /ready
GET /metrics
GET /api/v1/etfs
GET /api/v1/etfs/{symbol}/latest
GET /api/v1/etfs/{symbol}/bars?interval=1m|5m&from=...&to=...&limit=120
GET /api/v1/etfs/{symbol}/intraday?interval=1m|5m&date=YYYY-MM-DD
GET /api/v1/market/snapshot
WS  /ws/v1/market
GET /api/v1/system/status
GET /api/v1/etfs/XLK/prediction?date=YYYY-MM-DD
GET /api/v1/etfs/XLK/news?date=YYYY-MM-DD
GET /api/v1/etfs/XLK/insights?date=YYYY-MM-DD
GET /api/v1/etfs/XLK/nlp-features?date=YYYY-MM-DD
```

The legacy `/api/v1/recommendation` endpoint is retired. Predictions use only articles published
by the America/New_York market-close cutoff for the requested trading date.

For a local Kafka fixture run:

```bash
./build/local/news-ingestion --fixture tests/fixtures/xlk_news.jsonl
NEWS_TRADING_DATE=2026-07-28 \
NEWS_PREDICTION_CUTOFF_UNIX_MS=1785355200000 \
NEWS_PREDICTION_CUTOFF_ISO=2026-07-28T20:00:00Z \
ARRAKIS_FINBERT_ONNX_PATH="$PWD/models/finbert/model.onnx" \
ARRAKIS_FINBERT_VOCAB_PATH="$PWD/models/finbert/vocab.txt" \
./build/local/news-enricher
```

The news fixture requires Kafka and PostgreSQL migrations. It is intentionally separate from
production ingestion; no live provider credentials or fabricated news are used.

## Configuration and semantics

- `config/etf_universe.json` is the authoritative 18-symbol universe.
- All event and database timestamps are UTC.
- The default allowed lateness is 5 seconds.
- The default deduplication window is 15 minutes.
- Missing trading intervals are not synthesized or forward-filled.
- Trades arriving after a bar is finalized are counted as late and ignored in v1.
- Active and recent in-memory bars are reconstructed through Kafka replay after restart.
- The raw Kafka topic retains recent market data for replay; PostgreSQL stores ML/news research data.
