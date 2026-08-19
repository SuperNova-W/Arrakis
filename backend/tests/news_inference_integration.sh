#!/bin/sh
set -eu

backend_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
artifact=${ARRAKIS_XLK_NEWS_MODEL_PATH:-"$backend_root/artifacts/xlk_news_xgboost.json"}
manifest="$artifact.manifest.json"
predictions="$artifact.test_predictions.csv"
api_url=${ARRAKIS_MARKET_API_URL:-}

skip_test() {
    echo "SKIP: $1" >&2
    exit 77
}

if [ ! -f "$artifact" ] || [ ! -f "$manifest" ] || [ ! -f "$predictions" ]; then
    skip_test "trained XLK artifact, manifest, and held-out predictions are required"
fi

if [ -z "$api_url" ]; then
    skip_test "set ARRAKIS_MARKET_API_URL to run the REST portion against a live market-api"
fi

test_date=${ARRAKIS_INTEGRATION_TEST_DATE:-2023-12-28}
for endpoint in prediction news insights; do
    response=$(curl --fail --silent --show-error "$api_url/api/v1/etfs/XLK/$endpoint?date=$test_date")
    case "$response" in
        *"\"symbol\":\"XLK\""*) ;;
        *) echo "Integration response for $endpoint does not identify XLK" >&2; exit 1 ;;
    esac
    case "$response" in
        *"\"feature_schema_hash\":\"xlk-combined-features-v1\""*) ;;
        *) echo "Integration response for $endpoint has the wrong feature schema" >&2; exit 1 ;;
    esac
done

prediction=$(curl --fail --silent --show-error "$api_url/api/v1/etfs/XLK/prediction?date=$test_date")
case "$prediction" in
    *"\"prediction\":"*"\"direction\":"*) ;;
    *) echo "Prediction response did not contain a typed recommendation" >&2; exit 1 ;;
esac
echo "PASS: XLK news feature persistence and REST inference contract"
