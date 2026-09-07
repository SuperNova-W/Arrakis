#!/usr/bin/env bash
# Train the per-symbol market-feature candidate bundle locally.
#
# This is intentionally a candidate trainer: every generated manifest starts
# with promotion_eligible=false. A separate review of chronological
# walk-forward, calibration, and cost-aware results is required before a model
# can publish a prediction to the frontend.
set -euo pipefail

backend_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${ARRAKIS_BUILD_DIR:-"$backend_root/build/local"}
history_dir=${ARRAKIS_MARKET_HISTORY_DIR:-"$backend_root/data/history"}
output_dir=${ARRAKIS_MARKET_MODEL_OUTPUT_DIR:-"$backend_root/deploy/market_models"}
from_date=${ARRAKIS_MARKET_MODEL_FROM_DATE:-2016-01-01}
to_date=${ARRAKIS_MARKET_MODEL_TO_DATE:-2023-12-31}
train_end=${ARRAKIS_MARKET_MODEL_TRAIN_END:-2020-12-31}
validation_end=${ARRAKIS_MARKET_MODEL_VALIDATION_END:-2022-12-31}
test_end=${ARRAKIS_MARKET_MODEL_TEST_END:-2023-12-31}
symbols=${ARRAKIS_MARKET_MODEL_SYMBOLS:-"XLC XLY XLP XLE XLF XLV XLI XLB XLRE XLK XLU SPY QQQ IWM TLT HYG GLD USO"}

datasets=$(mktemp -d)
trap 'rm -rf "$datasets"' EXIT
mkdir -p "$output_dir"

for symbol in $symbols; do
    echo "===== BUILD $symbol DATASET ====="
    "$build_dir/arrakis-build-xlk-market-dataset" \
        "$history_dir" "$datasets/${symbol}.csv" "$from_date" "$to_date" "$symbol"
    echo "===== TRAIN $symbol MODEL ====="
    "$build_dir/arrakis-train-xgboost" \
        --input "$datasets/${symbol}.csv" \
        --symbol "$symbol" \
        --feature-subset market \
        --model-output "$output_dir/${symbol}.ubj" \
        --train-end "$train_end" \
        --validation-end "$validation_end" \
        --test-end "$test_end" \
        --rounds 75 \
        --early-stopping-rounds 20 \
        --seed 42
done

echo "Candidate models written to $output_dir"
