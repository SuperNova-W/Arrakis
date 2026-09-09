# XLK forecast deployment — 2026-09-09

The personal project now serves the existing validation-search FinBERT sentiment + XGBoost candidate, `xlk_news_xgboost_rebuilt_v2_hpo.json`, under the distinct deployed ID `xlk-finbert-xgboost-rebuilt-v2-hpo`. The user authorized displaying predictions without warning banners. Enabling inference is separate from research promotion: published documents retain `model_validated: false` and `prediction_status: experimental`. The UI displays direction and estimated probability, without claiming calibrated confidence or investment performance.

## Selection

This is the strongest ready-to-load candidate evaluated on the rebuilt 36-column next-close dataset. The 72-configuration search selected parameters on 2021–2022 validation log loss (0.710829, versus 0.712874 for the default candidate). Its 2023 test AUC was 0.540079 versus 0.500496 for the rebuilt default and 0.457937 for the formerly configured release artifact. Test log loss was 0.679620. The test period was already inspected during research; this deployment creates no new untouched evaluation claim.

The selected model contains one tree and predicts above 0.5 throughout the recorded test period. Test accuracy of 0.578313 equals the positive-class frequency; neither stable directional skill nor profitability is established. Higher scores in the later opening-gap experiments concern an after-open nowcast, not this next-close forecast. The 768-dimensional and shifted-news experiments require a different feature contract and have not been substituted into this pipeline.

The manifest records training (2019–2020), validation (2021–2022), test (2023), dataset checksum, feature order, threshold, frozen FinBERT/tokenizer hashes, model checksum and held-out predictions. The model, manifest, original metrics, search record and predictions are versioned in `backend/deploy/news_models`.

## Compatibility

The training manifest retains `xlk-combined-features-v1`. The live schema is `xlk-combined-features-v2`, with the same 36 names and order but corrected volatility and RSI calculations. Inspection of every non-leaf split proves this artifact uses neither changed column (indices 3 and 6). The compatibility verifier rejects any artifact that does. The eight reserved embedding columns (28–35) are also unused; the pinned ONNX export provides sentiment logits only. This is not the final full-embedding architecture.

The API checks the manifest target, symbol, runtime schema, complete feature order, threshold, finite input values and output probability. CI verifies the exact model checksum and FinBERT version pins before starting inference. No validation flag is manufactured to open the display path.

## Verification

- Frontend lint and production build pass.
- C++ market API builds with warnings treated as errors.
- Isolated PostgreSQL integration loads a held-out feature row, serves the selected model through the actual C++ API, and exports its prediction. Probability agrees with the saved held-out result within 0.000001; experimental status and false validation metadata survive export.
- Default disabled inference remains gated; missing features, database errors and malformed predictions remain operational failures.
- Live workflow and browser verification are recorded after publication below.
