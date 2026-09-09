# Inference delivery incident — 2026-09-08

The deployed Recommendations screen showed `NO_VALIDATED_MODEL`, zero articles, and duplicated
technical warnings. Browser inspection confirmed the Supabase read succeeded. Actions run
34290149833 reported success despite all 55 Finnhub news requests failing JSON parsing.

## Causes and repair

- `FinnhubClient` ignored its configured `https://finnhub.io/api/v1` base URL and requested website
  paths `/company-news` and `/stock/candle`. The website redirected to HTML. Requests now use the
  configured host and API prefix; regression tests exercise the actual client through a fake transport.
- Scheduled ingestion treated `poll_complete` as success even when all requests failed. New
  `--finnhub-once` requires all provider requests to succeed, flushes Kafka, and exits. An explicit
  `NEWS_TRADING_DATE` also makes backfills fetch the requested date's news and constituents.
- Enrichment errors and timeouts were swallowed; a pre-existing feature row made subsequent runs
  appear successful. The workflow now requires the batch to exit successfully. Enrichment failures
  abort batch execution rather than publishing partial success.
- Exporters accepted arbitrary HTTP errors as valid research documents. Only the explicit
  `NO_VALIDATED_MODEL` refusal is now publishable; operational errors fail without replacing the
  public result. Shell regression tests cover model, database, HTML and transport failures.
- `/api/v1/etfs/XLK/prediction` was shadowed by the generic market-model route. It now shares the
  FinBERT/news path with `/insights`. Unsupported hardcoded news themes were removed.
- The UI now uses plain-language availability states, actual dates, explicit latest/exact-date modes,
  safe article citations, readable text, visible keyboard focus and a chart table alternative.
  Responses are keyed to the selected fund/date to prevent displaying a previous fund's result.

## Operational checks

Run `Hourly ETF signal` for a completed trading day. Confirm ingestion exits successfully, with
`failed_requests: 0`; enrichment exits successfully; export includes article citations; and the
read-only public result and deployed browser show the new update. Empty successful provider
responses remain a valid no-news outcome. An ingestion failure must stop publication.

The model validation gate remains closed. A functioning news/inference delivery path is distinct
from evidence that a forecast is reliable. Enabling forecasts requires the existing documented
validation evidence, not merely changing a flag to hide an empty screen.

Local verification: C++ client regression test, daily-bar/news/market-feature tests, exporter failure
regressions, frontend lint and build. Live deployment verification is recorded separately after the
repaired workflow completes.
