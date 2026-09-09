#!/usr/bin/env bash
# Export one market-model research signal per configured ETF.
#
# The market-only baseline is deliberately separate from the XLK FinBERT news
# document. Each symbol has its own artifact, manifest, and promotion gate. A
# gated candidate produces a document with prediction_error rather than an
# unsupported prediction, so Supabase and the frontend can distinguish
# "published candidate" from "validated model".
set -euo pipefail

api_url=${ARRAKIS_MARKET_API_URL:-http://127.0.0.1:8080}
signal_date=${ARRAKIS_SIGNAL_DATE:?ARRAKIS_SIGNAL_DATE must be set (YYYY-MM-DD)}
out_dir=${ARRAKIS_SIGNAL_OUT_DIR:?ARRAKIS_SIGNAL_OUT_DIR must be set}
symbols=${ARRAKIS_SIGNAL_SYMBOLS:-"XLC XLY XLP XLE XLF XLV XLI XLB XLRE XLK XLU SPY QQQ IWM TLT HYG GLD USO"}
git_sha=${GITHUB_SHA:-$(git rev-parse HEAD 2>/dev/null || echo unknown)}
run_kind=${ARRAKIS_SIGNAL_RUN_KIND:-intraday}
window_start=${ARRAKIS_SIGNAL_WINDOW_START_ISO:-}
publication_cutoff=${ARRAKIS_SIGNAL_CUTOFF_ISO:-${signal_date}T23:59:59Z}
run_url=${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY:-local}/actions/runs/${GITHUB_RUN_ID:-0}

mkdir -p "$out_dir"

for symbol in $symbols; do
    body=$(mktemp)
    trap 'rm -f "$body"' EXIT
    status=$(curl --silent --show-error --max-time 30 \
        --output "$body" --write-out '%{http_code}' \
        "${api_url}/api/v1/etfs/${symbol}/prediction?date=${signal_date}")

    if [ "$status" = "200" ]; then
        document=$(jq \
            --arg generated_at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
            --arg git_sha "$git_sha" \
            --arg run_kind "$run_kind" \
            --arg window_start "$window_start" \
            --arg run_url "$run_url" \
            '(.articles // []) |= map(del(.body))
             | . + {
                generated_at: $generated_at,
                source_commit: $git_sha,
                run_kind: $run_kind,
                signal_of_record: ($run_kind == "post_close"),
                window_start: (if $window_start == "" then null else $window_start end),
                pipeline_run: $run_url
             }' "$body")
    else
        if [ "$status" != "503" ] || ! jq -e '.error.code == "NO_VALIDATED_MODEL"' "$body" >/dev/null; then
            echo "FATAL: ${symbol} prediction returned an unexpected failure (HTTP ${status})." >&2
            exit 1
        fi
        code=$(jq -r '.error.code // empty' "$body" 2>/dev/null || true)
        code=${code:-MODEL_UNAVAILABLE}
        message=$(jq -r '.error.message // empty' "$body" 2>/dev/null || true)
        message=${message:-The market prediction endpoint is unavailable.}
        document=$(jq -n \
            --arg symbol "$symbol" \
            --arg date "$signal_date" \
            --arg code "$code" \
            --arg message "$message" \
            --arg generated_at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
            --arg git_sha "$git_sha" \
            --arg run_kind "$run_kind" \
            --arg window_start "$window_start" \
            --arg publication_cutoff "$publication_cutoff" \
            --arg run_url "$run_url" \
            --argjson http_status "${status:-0}" \
            '{
                symbol: $symbol,
                date: $date,
                publication_cutoff: $publication_cutoff,
                coverage_status: "empty",
                feature_schema_hash: "market-features-v1",
                articles: [],
                prediction_error: {code: $code, message: $message, http_status: $http_status},
                model_validated: false,
                generated_at: $generated_at,
                source_commit: $git_sha,
                run_kind: $run_kind,
                signal_of_record: ($run_kind == "post_close"),
                window_start: (if $window_start == "" then null else $window_start end),
                pipeline_run: $run_url,
                research_only_disclaimer: "Research signals only. Not investment advice. No trades are executed by this platform."
            }')
    fi

    printf '%s\n' "$document" >"${out_dir}/${symbol}-${signal_date}.json"
    printf '%s\n' "$document" >"${out_dir}/${symbol}-latest.json"
    jq -r --arg symbol "$symbol" \
        '"wrote " + $symbol + " " + .date + " model=" + (if .prediction then .prediction.model_id else "none (" + .prediction_error.code + ")" end)' \
        <<<"$document" >&2
    rm -f "$body"
    trap - EXIT
done
