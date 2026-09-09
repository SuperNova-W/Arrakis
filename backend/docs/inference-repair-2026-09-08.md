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
- The article query used `SELECT DISTINCT` while ordering by an unselected timestamp. PostgreSQL
  rejected it, and unchecked query status converted the error into zero citations. It now uses
  `EXISTS`, checks both query results, and restricts citations to midnight New York through the
  snapshot cutoff. An isolated PostgreSQL/API regression verifies boundaries, uniqueness, route
  selection and error propagation. Export also rejects positive news counts with no citations.
- A keepalive push could race a deployment commit. It now rebases the heartbeat on the current
  branch before pushing, preserving intervening code changes.
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
regressions, frontend lint/build and an isolated PostgreSQL 16 integration test. Browser verification
covered the deployed recommendations, exact-date empty state, fund chart and price-table alternative;
390px mobile selectors measure 358px wide and 44px high without page overflow.

The first repaired run (34303213766) fetched 814 distinct articles with zero failed requests across
55 provider calls and completed FinBERT enrichment. It exposed the citation query error above; its
heartbeat also raced a frontend deployment. A subsequent run verifies those follow-up fixes.
