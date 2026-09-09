# All-sector FinBERT + XGBoost artifacts (2026-09-09)

The deployed research path now has one model artifact for every sector ETF in
`config/etf_universe.json`: XLB, XLC, XLE, XLF, XLI, XLK, XLP, XLRE, XLU, XLV, and XLY.
Each artifact uses the same frozen FinBERT ONNX model for article scoring and a separate
XGBoost head for that ETF's next-close direction. FinBERT is intentionally shared and versioned;
these are sector-specific prediction heads, not eleven separately fine-tuned language models.

## Data and provenance

Historical article inputs come from the normalized FNSPID panel at
`backend/data/fnspid/normalized/sector_articles.csv`. The importer assigns articles to the first
SPY session after publication and joins them to the point-in-time SEC N-PORT holdings history.
The coverage audit records 260,560 rows, 1,026 eligible sessions, and all 11 sectors active on at
least one date (`backend/data/fnspid/manifests/sector_coverage_audit.json`). The live ingestion path
uses Finnhub `company-news`; its sector constituent filter is limited by the holdings history
available to the deployment and should be treated as a direct ETF/company-news feed until a
sector-complete live holdings resolver is installed.

The local FinBERT artifact is `arrakis-finbert-v1`, fetched from the pinned `models-v1` release
(`model.onnx`, `vocab.txt`, and the separately published `model_with_pooled_embedding.onnx`) and
checked against `backend/models/finbert/manifest.json`. The production base graph SHA-256 is
`4a8d58ba2f8d74c7fca30fdb49fbbe367b64760104b64e8623e896a007229a6e`; the tokenizer SHA-256 is
`07eced375cec144d27c900241f3e339dec958f92fddbc551f295c992038a3`.
The live workflow verifies the pooled graph SHA-256
`c7f8304257b2a587d9d9b348410b3809cc9403da909cc0331a63294426e4205a` before starting inference.

## Local training run

The local host generated deterministic, evenly spaced samples capped at 1,000 article rows per
sector. The cap is recorded in each model manifest because scoring the complete 204,000-input
FNSPID corpus exceeds the practical runtime of this workstation. Every retained input was
deduplicated by the exact `title + " " + summary` string, tokenized to 64 tokens, scored by the
frozen pooled-embedding FinBERT graph, and joined with the same market/context feature builder.
The XGBoost heads use chronological windows (2019–2020 train, 2021–2022 validation, 2023 test)
and the fixed 36-column combined schema. Their manifests retain dataset checksums, feature order,
FinBERT/tokenizer hashes, thresholds, and held-out predictions.

All eleven artifacts are marked `model_validated: false`, `promotion_eligible: false`, and
`deployment_status: experimental`. They are enabled for the personal research UI so the dashboard
can show the measured model output, but the run is not evidence of a predictive edge and must not
be presented as a trading recommendation.
