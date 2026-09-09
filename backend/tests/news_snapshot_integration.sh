#!/usr/bin/env bash
# Isolated PostgreSQL regression for the actual C++ API query. No production writes.
set -euo pipefail
command -v docker >/dev/null && command -v jq >/dev/null || exit 77
docker info >/dev/null 2>&1 || exit 77
backend_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
api_binary=${1:?market-api binary is required}
api_port=${ARRAKIS_NEWS_QUERY_TEST_PORT:-18089}
task_tmp=$(mktemp -d)
container_id=''
api_pid=''
cleanup() {
    [ -z "$api_pid" ] || kill "$api_pid" 2>/dev/null || true
    [ -z "$container_id" ] || docker rm -f "$container_id" >/dev/null
    rm -rf "$task_tmp"
}
trap cleanup EXIT
container_id=$(docker run --rm -d -e POSTGRES_PASSWORD=test-only -p 127.0.0.1::5432 postgres:16-alpine)
for _ in {1..30}; do
    docker exec "$container_id" pg_isready -U postgres >/dev/null 2>&1 && break
    sleep 1
done
pg_port=$(docker port "$container_id" 5432/tcp | sed 's/.*://')
for migration in V001__create_etf_metadata.sql V007__create_news_nlp_tables.sql; do
    docker exec -i "$container_id" psql -U postgres -v ON_ERROR_STOP=1 -q < "$backend_root/migrations/$migration"
done
docker exec -i "$container_id" psql -U postgres -v ON_ERROR_STOP=1 -q <<'SQL'
INSERT INTO etf_metadata(symbol,name,category) VALUES ('XLK','Technology','sector');
INSERT INTO etf_daily_news_features(symbol,trading_date,cutoff_timestamp,feature_schema_hash,features,article_count,coverage_status)
VALUES ('XLK','2026-09-08','2026-09-08T20:00:00Z','test-schema','{"article_count":2}',2,'complete');
INSERT INTO news_articles(article_id,canonical_url,normalized_content_hash,source_id,headline,published_at,retrieved_at)
VALUES ('eligible','https://example.com/eligible','hash1','Example','Eligible news','2026-09-08T15:00:00Z','2026-09-08T15:05:00Z'),
('opening-boundary','https://example.com/boundary','hash2','Example','Midnight news','2026-09-08T04:00:00Z','2026-09-08T04:05:00Z'),
('too-old','https://example.com/old','hash3','Example','Prior day','2026-09-08T03:59:59Z','2026-09-08T04:05:00Z'),
('too-new','https://example.com/new','hash4','Example','After cutoff','2026-09-08T20:00:01Z','2026-09-08T20:05:00Z');
INSERT INTO news_article_entities(article_id,entity_type,entity_id) SELECT article_id,'etf','XLK' FROM news_articles;
INSERT INTO news_article_entities(article_id,entity_type,entity_id) VALUES ('eligible','company','company:MSFT');
SQL
SUPABASE_DB_URL="postgresql://postgres:test-only@127.0.0.1:${pg_port}/postgres" \
ARRAKIS_ETF_UNIVERSE="$backend_root/config/etf_universe.json" \
ARRAKIS_XLK_NEWS_MODEL_VALIDATED=false ARRAKIS_MARKET_MODEL_DIR='' ARRAKIS_FEATURE_SCHEMA_HASH=test-schema \
KAFKA_BOOTSTRAP_SERVERS=127.0.0.1:1 MARKET_API_PORT="$api_port" \
"$api_binary" > "$task_tmp/api.log" 2>&1 &
api_pid=$!
for _ in {1..30}; do
    curl -fsS "http://127.0.0.1:${api_port}/health" >/dev/null 2>&1 && break
    kill -0 "$api_pid"
    sleep 1
done
url="http://127.0.0.1:${api_port}/api/v1/etfs/XLK"
curl -fsS "$url/news?date=2026-09-08" > "$task_tmp/news.json"
jq -e '[.articles[].article_id] == ["eligible","opening-boundary"]' "$task_tmp/news.json" >/dev/null
# The XLK prediction route must use this news snapshot, not a market-only artifact.
curl -sS "$url/prediction?date=2026-09-08" | jq -e '.error.code == "NO_VALIDATED_MODEL"' >/dev/null
# Break only our disposable schema: database errors must be HTTP failures, not empty arrays.
docker exec "$container_id" psql -U postgres -v ON_ERROR_STOP=1 -qc 'DROP TABLE news_nlp_features'
status=$(curl -sS -o "$task_tmp/error.json" -w '%{http_code}' "$url/news?date=2026-09-08")
[ "$status" = 500 ]
echo 'PASS: citations respect the daily window, remain unique, and query failures reach the caller'
