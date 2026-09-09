#!/usr/bin/env bash
# Verify the exact deployment artifact and the audited v1 -> v2 compatibility.
set -euo pipefail
backend_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
model=${1:-"$backend_root/deploy/news_models/XLK.json"}
manifest="$model.manifest.json"
actual=$(shasum -a 256 "$model" | cut -d' ' -f1)
[ "$actual" = "$(jq -r '.model_sha256' "$manifest")" ] || { echo 'Model checksum mismatch' >&2; exit 1; }
jq -e '.symbol == "XLK" and .model_type == "xgboost" and .target == "target_next_close_up" and
  .runtime_feature_schema_hash == "xlk-combined-features-v2" and
  .feature_schema_hash == "xlk-combined-features-v1" and (.feature_names | length == 36)' "$manifest" >/dev/null
# v2 corrects volatility and RSI. This model never uses either feature, so the
# correction cannot change a tree decision. The logits-only ONNX export also
# matches training: the eight reserved embedding columns are unused.
jq -e '.learner.learner_model_param.num_feature == "36" and
  .learner.objective.name == "binary:logistic" and
  ([.learner.gradient_booster.model.trees[] | . as $tree |
    range(0; .split_indices | length) | select($tree.left_children[.] != -1) |
    $tree.split_indices[.] | select(. == 3 or . == 6 or . >= 28)] | length == 0)' "$model" >/dev/null
jq -e --slurpfile finbert "$backend_root/models/finbert/manifest.json" '
  .finbert_model_sha256 == $finbert[0].model_sha256 and
  .tokenizer_sha256 == $finbert[0].vocabulary_sha256' "$manifest" >/dev/null
echo 'PASS: XLK artifact checksum, target, schema compatibility and FinBERT version'
