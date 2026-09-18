# Per-ETF market model bundle

This directory contains one independently trained XGBoost market-feature model
for every ETF in `config/etf_universe.json`. Each symbol has a model artifact,
manifest, held-out test metrics, and held-out prediction file.

The artifacts were trained locally from `backend/data/history` with features
through close[t] and the target `close[t+1] > close[t]`. The split is
chronological: training through 2020, validation through 2022, and held-out
testing in 2023.

Every manifest is marked `promotion_eligible: false` because these candidates
did not clear the independent walk-forward promotion bar. The deployment
runtime loads and verifies every candidate and, unless
`ARRAKIS_MARKET_MODEL_ENABLED=true`, publishes `NO_VALIDATED_MODEL` for it.

With that flag set the candidate's forecast is published, but the gate is still
reported rather than bypassed: the document carries `model_validated: false`
and `prediction_status: "experimental"`. Only updating a manifest's
`promotion_eligible` after reproducible validation makes a document validated.

These are market-only baseline candidates. They do not replace the approved
FinBERT plus XGBoost news model path for XLK.
