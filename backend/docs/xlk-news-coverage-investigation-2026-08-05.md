# XLK point-in-time news coverage investigation

Date: 2026-08-05

## Finding

No news-source expansion was implemented in this pass. The current offline path already
uses historical constituent news rather than only current XLK holdings:

- `backend/data/fnspid/manifests/import.json` records 71,654 normalized rows written
  after point-in-time membership filtering and deduplication.
- `backend/data/metadata/xlk_holdings_history.csv` begins on 2019-09-30 and is sourced
  from quarterly SEC N-PORT snapshots. The importer applies each snapshot only from its
  effective date until the next snapshot and explicitly avoids a current-holdings
  fallback.
- `build_xlk_combined_dataset` aggregates eligible historical constituent articles into
  one XLK feature vector per trading day. The 1,251-row combined file therefore has 1,251
  daily labels; adding more articles on an already-covered day improves coverage but does
  not create independent target observations.

The batch builder and streaming enricher both apply publication cutoffs. The batch path
keeps an article only when its publication timestamp is no later than that trading day’s
market close. The streaming enricher also rejects articles later than the configured
prediction cutoff before FinBERT scoring and persistence.

## Feasibility and estimated gain

The local XLK market history contains 611 additional trading sessions before 2019-01-01,
starting in 2016-07. If an independently sourced, point-in-time XLK constituent history
and eligible news coverage were validated for that period, the daily feature sample could
theoretically grow from 1,251 to about 1,862 rows, a maximum gain of roughly 49%. The
actual gain would be lower if the news source has gaps or if lookback/forward-target
alignment removes boundary rows.

That expansion is currently blocked by data provenance, not model code: the checked-in
holdings history starts in 2019-09-30, and the SEC N-PORT snapshots in the repository do
not supply the earlier membership intervals. The example holdings file is explicitly an
example and cannot be used for evaluation. A pre-2019 source such as an auditable fund
sponsor or index-history archive would need to be acquired, normalized, and validated
before any earlier rows could be included.

Constituent coverage is already present in the offline FNSPID import, so switching from
“XLK-only” to major constituents would not add daily labels to the current 2019–2023
training file. It would add value only if the system retained company-level features or
expanded the historical date range. The live Finnhub path is narrower: `news_enricher`
currently accepts only articles carrying the `XLK` entity, while company entities are
stored separately. Supporting live constituent news would require a point-in-time
membership resolver and an ETF association before aggregation; accepting current
constituents without that resolver would introduce survivorship/lookahead risk.

## Decision

Do not add a new source or alter the feature schema yet. First source and validate
pre-2019 historical XLK membership, then rebuild the same point-in-time aggregation and
rerun walk-forward evaluation. Treat the 611-session figure as an upper-bound estimate,
not as a measured sample-size gain until the membership and news joins are reproducible.
