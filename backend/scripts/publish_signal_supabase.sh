#!/usr/bin/env bash
# Publish one research document into Supabase.
#
# This replaces committing JSON to the repository. The document produced by
# export_daily_signal.sh is upserted into research_signals (see
# migrations/V009__create_research_signal_surface.sql), and the browser reads it
# back through the public_research_signal_latest view using the anon key.
#
# Writing goes over the direct Postgres connection (SUPABASE_DB_URL) rather than
# PostgREST, because the anon key must never carry write permission and the
# service-role key is not needed for a plain INSERT from CI.
#
# Every run inserts a new row keyed by (symbol, trading_date, generated_at), so
# the intraday sequence for a day is preserved and auditable. The view collapses
# them to the newest per date for display.
set -euo pipefail

document_path=${1:?usage: publish_signal_supabase.sh <document.json>}
db_url=${SUPABASE_DB_URL:?SUPABASE_DB_URL must be set}
retain_days=${ARRAKIS_SIGNAL_RETAIN_DAYS:-120}

[ -f "$document_path" ] || { echo "FATAL: no document at $document_path" >&2; exit 1; }

# Pull the indexed columns out of the document itself so the row and the JSONB
# can never disagree.
symbol=$(jq -r '.symbol // "XLK"' "$document_path")
trading_date=$(jq -r '.date' "$document_path")
generated_at=$(jq -r '.generated_at' "$document_path")
run_kind=$(jq -r '.run_kind // "intraday"' "$document_path")
cutoff=$(jq -r '.publication_cutoff' "$document_path")
window_start=$(jq -r '.window_start // .publication_cutoff' "$document_path")
coverage=$(jq -r '.coverage_status // "empty"' "$document_path")
article_count=$(jq -r '(.articles // []) | length' "$document_path")
model_validated=$(jq -r '.model_validated // false' "$document_path")
schema_hash=$(jq -r '.feature_schema_hash // "unknown"' "$document_path")
source_commit=$(jq -r '.source_commit // ""' "$document_path")
pipeline_run=$(jq -r '.pipeline_run // ""' "$document_path")

for required in "$trading_date" "$generated_at" "$cutoff"; do
    if [ -z "$required" ] || [ "$required" = "null" ]; then
        echo "FATAL: document is missing a required field (date/generated_at/publication_cutoff)." >&2
        exit 1
    fi
done

# The document is passed as a psql variable rather than interpolated into the
# SQL text, so quotes and apostrophes inside headlines cannot break the
# statement or inject anything.
psql "$db_url" -v ON_ERROR_STOP=1 -q \
    -v symbol="$symbol" \
    -v trading_date="$trading_date" \
    -v generated_at="$generated_at" \
    -v run_kind="$run_kind" \
    -v cutoff="$cutoff" \
    -v window_start="$window_start" \
    -v coverage="$coverage" \
    -v article_count="$article_count" \
    -v model_validated="$model_validated" \
    -v schema_hash="$schema_hash" \
    -v source_commit="$source_commit" \
    -v pipeline_run="$pipeline_run" \
    -v document="$(cat "$document_path")" \
    <<'SQL'
INSERT INTO research_signals (
    symbol, trading_date, generated_at, run_kind, cutoff_timestamp,
    window_start_timestamp, coverage_status, article_count, model_validated,
    feature_schema_hash, source_commit, pipeline_run, document
) VALUES (
    :'symbol', :'trading_date'::date, :'generated_at'::timestamptz, :'run_kind',
    :'cutoff'::timestamptz, :'window_start'::timestamptz, :'coverage',
    :'article_count'::integer, :'model_validated'::boolean, :'schema_hash',
    NULLIF(:'source_commit', ''), NULLIF(:'pipeline_run', ''), :'document'::jsonb
)
ON CONFLICT (symbol, trading_date, generated_at) DO UPDATE SET
    run_kind               = EXCLUDED.run_kind,
    cutoff_timestamp       = EXCLUDED.cutoff_timestamp,
    window_start_timestamp = EXCLUDED.window_start_timestamp,
    coverage_status        = EXCLUDED.coverage_status,
    article_count          = EXCLUDED.article_count,
    model_validated        = EXCLUDED.model_validated,
    feature_schema_hash    = EXCLUDED.feature_schema_hash,
    source_commit          = EXCLUDED.source_commit,
    pipeline_run           = EXCLUDED.pipeline_run,
    document               = EXCLUDED.document;
SQL

psql "$db_url" -v ON_ERROR_STOP=1 -q -c "SELECT prune_research_history(${retain_days});"

echo "Published ${symbol} ${trading_date} (${run_kind}, ${article_count} articles, validated=${model_validated})." >&2
