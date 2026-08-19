#!/usr/bin/env bash
# Export one day's XLK research signal as a static JSON document.
#
# This is the last step of the zero-cost daily pipeline. By the time it runs,
# news-enricher has written etf_daily_news_features for the trading date and
# market-api is serving on $ARRAKIS_MARKET_API_URL.
#
# It queries the SAME public endpoints the browser used to call, so the static
# document and the live API cannot drift in shape:
#
#   GET /api/v1/etfs/XLK/news?date=D      -> available once features exist
#   GET /api/v1/etfs/XLK/insights?date=D  -> 200 with a prediction ONLY when a
#                                            validated model is enabled, else
#                                            503 NO_VALIDATED_MODEL
#
# A 503 from /insights is a NORMAL, EXPECTED outcome, not a failure. The gate
# ARRAKIS_XLK_NEWS_MODEL_VALIDATED stays false until a target clears the
# documented walk-forward bar, and AGENTS.md forbids presenting a prediction
# without that evidence. With the gate closed we publish the news/feature
# payload plus an explicit prediction_error, and the existing UI renders its
# "No validated model" state.
set -euo pipefail

api_url=${ARRAKIS_MARKET_API_URL:-http://127.0.0.1:8080}
signal_date=${ARRAKIS_SIGNAL_DATE:?ARRAKIS_SIGNAL_DATE must be set (YYYY-MM-DD)}
out_dir=${ARRAKIS_SIGNAL_OUT_DIR:?ARRAKIS_SIGNAL_OUT_DIR must be set}
git_sha=${GITHUB_SHA:-$(git rev-parse HEAD 2>/dev/null || echo unknown)}

mkdir -p "$out_dir"

news_body=$(mktemp)
insights_body=$(mktemp)
trap 'rm -f "$news_body" "$insights_body"' EXIT

# Capture body and status separately; a non-2xx must not abort the script.
fetch() {
    curl --silent --show-error --max-time 30 \
        --output "$2" --write-out '%{http_code}' \
        "${api_url}/api/v1/etfs/XLK/$1?date=${signal_date}" || printf '000'
}

news_status=$(fetch news "$news_body")
insights_status=$(fetch insights "$insights_body")
echo "news=${news_status} insights=${insights_status}" >&2

if [ "$news_status" != "200" ]; then
    echo "FATAL: /news returned ${news_status}; there is no feature snapshot to publish." >&2
    cat "$news_body" >&2 || true
    exit 1
fi

if [ "$insights_status" = "200" ]; then
    # Validated model enabled: /insights is a superset of /news.
    merged=$(jq -s '.[0] * .[1] + {prediction_error: null}' "$news_body" "$insights_body")
else
    # Gate closed (expected). Record the refusal verbatim; guarantee that
    # `prediction` is absent rather than present-and-null.
    # NOTE: bind the slurped array before piping to .[0]; once you pipe, the
    # array context is gone and .[1] would index an object. (Caught by test.)
    merged=$(jq -s \
        --argjson http "${insights_status:-0}" \
        '. as $in
         | $in[0]
         | del(.prediction)
         | .prediction_error = {
             code:        ($in[1].error.code    // "MODEL_UNAVAILABLE"),
             message:     ($in[1].error.message // "The prediction endpoint returned HTTP \($http)."),
             http_status: $http
           }' "$news_body" "$insights_body")
fi

# Provenance: every published document says what produced it, from which commit,
# and when -- so a stale page is detectable rather than silently wrong.
#
# run_kind distinguishes the post-close signal of record from provisional
# intraday runs. Both are point-in-time correct (the enricher's cutoff never
# admits an article published after it), but only the post-close run has the
# full-day news window the model was trained on. The UI must not present an
# intraday reading as equivalent evidence.
#
# Article bodies are DELETED here. They are the bulk of the payload, they are
# licensed provider text, and nothing in the UI renders them -- only headline,
# source, link and sentiment are shown.
document=$(jq \
    --arg generated_at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    --arg git_sha "$git_sha" \
    --arg gate "${ARRAKIS_XLK_NEWS_MODEL_VALIDATED:-false}" \
    --arg run_kind "${ARRAKIS_SIGNAL_RUN_KIND:-intraday}" \
    --arg window_start "${ARRAKIS_SIGNAL_WINDOW_START_ISO:-}" \
    --arg run_url "${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY:-local}/actions/runs/${GITHUB_RUN_ID:-0}" \
    '(.articles // []) |= map(del(.body))
     | . + {
        generated_at:  $generated_at,
        source_commit: $git_sha,
        model_validated: ($gate == "true"),
        run_kind:      $run_kind,
        signal_of_record: ($run_kind == "post_close"),
        window_start:  (if $window_start == "" then null else $window_start end),
        pipeline_run:  $run_url
     }' <<<"$merged")

printf '%s\n' "$document" >"${out_dir}/${signal_date}.json"
printf '%s\n' "$document" >"${out_dir}/latest.json"

jq -r '"wrote \(.date) coverage=\(.coverage_status) articles=\(.articles | length) prediction=" +
       (if .prediction then .prediction.direction else "none (" + .prediction_error.code + ")" end)' \
    <<<"$document" >&2
