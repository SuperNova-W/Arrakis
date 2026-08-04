#!/bin/sh
set -eu

backend_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${ARRAKIS_BUILD_DIR:-"$backend_root/build/local"}
raw_news=${ARRAKIS_FNSPID_RAW:-"$backend_root/data/fnspid/raw/nasdaq_exteral_data.csv"}
holdings=${ARRAKIS_XLK_HOLDINGS:-"$backend_root/data/metadata/xlk_holdings_history.csv"}
normalized="$backend_root/data/fnspid/normalized/xlk_articles.csv"
manifest="$backend_root/data/fnspid/manifests/import.json"
combined="$backend_root/data/fnspid/normalized/xlk_combined_features.csv"
artifact="$backend_root/artifacts/xlk_news_xgboost.json"

if [ ! -f "$raw_news" ]; then
    echo "FNSPID raw file is missing: $raw_news" >&2
    exit 1
fi
if [ ! -f "$holdings" ]; then
    echo "Historical XLK holdings are missing: $holdings" >&2
    exit 1
fi

raw_bytes=$(wc -c < "$raw_news" | tr -d ' ')
if [ "$raw_bytes" -lt 20000000000 ]; then
    echo "FNSPID download is incomplete: ${raw_bytes} bytes" >&2
    exit 1
fi

cd "$backend_root"
"$build_dir/services/ml_model/arrakis-import-fnspid" \
    --input "$raw_news" \
    --holdings "$holdings" \
    --output "$normalized" \
    --manifest "$manifest" \
    --from 2019-01-01 \
    --to 2023-12-31

(
    cd "$backend_root"
    "$build_dir/services/news_nlp/arrakis-build-xlk-combined" \
        "$normalized" "$backend_root/data/history" "$combined"
)

mkdir -p "$(dirname -- "$artifact")"
ARRAKIS_FEATURE_SCHEMA_HASH=xlk-combined-features-v1 \
    "$build_dir/arrakis-train-xgboost" \
        --input "$combined" \
        --model-output "$artifact" \
        --train-end 2020-12-31 \
        --validation-end 2022-12-31 \
        --test-end 2023-12-31 \
        --rounds 75
