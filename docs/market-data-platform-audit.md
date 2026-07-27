# Market-data platform audit

Date: 2026-07-26

## Baseline verification

- `cmake --preset debug` succeeds.
- `cmake --build --preset debug` succeeds with no pending work.
- `ctest --preset debug --output-on-failure` passes all 6 existing tests:
  `arrakis_model_core_tests`, `arrakis_market_core_tests`,
  `arrakis_historical_data_tests`, `arrakis_streaming_tests`,
  `feature_engine_tests`, and `prediction_service_tests`.
- The `core-only` preset configures and builds, but intentionally registers no tests because it
  disables both the market stream and XGBoost targets.

## Current component classification

| Area | Current state | Action for market-data objective |
|---|---|---|
| Finnhub WebSocket parsing/TLS | Implemented in `libs/market` and `src/market_stream.cpp` | Reuse; split executable/config naming into `market-ingestion` |
| Trade protobuf and deterministic identity | Implemented and covered by market tests | Reuse; verify schema fields and SHA-256 identity requirements |
| Kafka producer/consumer | Implemented in shared streaming library | Reuse; restrict initial deployment to `market.raw.trades` only |
| One-minute event-time aggregation | Implemented in `services/bar_aggregator` | Refactor output boundary to PostgreSQL and add five-minute derivation |
| PostgreSQL | Not implemented | Add database adapter, migrations, transactions, and idempotent bar writes |
| REST API | Not implemented | Add C++20 read-only PostgreSQL API with `/api/v1` endpoints |
| React UI | Existing mock research console in `frontend/` | Replace market-data panels with API-backed loading/error/stale states |
| Feature engine / prediction service | Implemented but outside current objective | Defer from runtime and compose; preserve source for future ML phase |
| Docker Compose | Kafka + stream/ML services, no PostgreSQL/API/UI deployment | Replace with the requested single-Kafka-boundary stack |
| Prometheus | Basic metrics server/config present | Reuse metrics primitives; add service-specific metrics and scrape targets |
| Grafana / Kafka UI | Partial local services present | Retain and complete health checks/configuration |
| Configuration | ETF universe currently duplicated in ingestion config | Make `config/etf_universe.json` authoritative and remove symbol duplication |

## Risks and unsafe abstractions found

1. The current compose file creates completed-bar, late-trade, feature, prediction, and dead-letter
   Kafka topics. This violates the new single-boundary requirement and must be removed from the
   initial runtime.
2. The current bar aggregator publishes finalized bars but has no PostgreSQL transaction/offset
   ordering. The replacement must persist first and commit Kafka offsets second.
3. The current bar state is in memory. Replay recovery is acceptable for the first version, but
   active bars and the five-minute derivation must be deterministic after restart.
4. There is no API or database contract yet, so the UI cannot truthfully display live bar
   freshness. Mock signal/recommendation data must be removed from the market-data UI.
5. Existing ML services are working but are out of scope for this phase; enabling them would add
   Kafka boundaries explicitly prohibited by the request.

## Incremental implementation plan

1. Make ETF configuration canonical and add trade/bar schemas and SQL migrations.
2. Extract reusable PostgreSQL and bar-persistence libraries; add deterministic 1m/5m unit tests.
3. Refactor `bar-aggregator` to persist completed bars and commit offsets after transaction commit.
4. Add the read-only C++ `market-api` and health/readiness/metrics endpoints.
5. Replace Docker Compose with Kafka, PostgreSQL, migration, ingestion, aggregator, API, UI,
   Prometheus, Grafana, and Kafka UI, with only `market.raw.trades` as the application topic.
6. Replace mock UI data with API query hooks and explicit loading, stale, empty, and unavailable
   states.
7. Add integration tests for replay/idempotency, bar derivation, API queries, and compose health.

No exactly-once claim is made. The intended delivery semantics are at-least-once Kafka processing
with deterministic IDs and idempotent PostgreSQL writes.

## Work completed after audit

- Added the canonical 18-symbol `sector_etfs` / `context_etfs` configuration shape.
- Added PostgreSQL migrations for ETF metadata, 1-minute bars, 5-minute bars, indexes, and seed
  data. All timestamps use `TIMESTAMPTZ`.
- Added the requested environment-variable contract to `backend/.env.example`.
- Extended the reusable C++ bar aggregator to derive 5-minute bars from finalized 1-minute bars,
  with a regression test covering OHLCV and trade-count derivation.
- Rebuilt and reran the existing suite: all 6 tests pass.

The PostgreSQL adapter, transaction/offset boundary, C++ REST API, and API-backed UI remain the
next implementation slice. They are not represented as complete by this audit.
