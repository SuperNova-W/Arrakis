#!/usr/bin/env bash
# Exercise production exporters against a fake HTTP transport: no database writes.
set -euo pipefail
command -v jq >/dev/null || exit 77
backend_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
task_tmp=$(mktemp -d)
trap 'rm -rf "$task_tmp"' EXIT
mkdir -p "$task_tmp/bin" "$task_tmp/output"
cat > "$task_tmp/bin/curl" <<'CURL'
#!/usr/bin/env bash
set -euo pipefail
output=''
url=''
while [ "$#" -gt 0 ]; do
    case "$1" in
        --output) output=$2; shift 2 ;;
        http*) url=$1; shift ;;
        *) shift ;;
    esac
done
if [[ "$url" == *'/news?'* ]]; then
    printf '%s' '{"symbol":"XLK","date":"2026-09-08","publication_cutoff":"2026-09-08T20:00:00Z","coverage_status":"complete","articles":[{"headline":"A headline","body":"licensed body"}]}' > "$output"
    printf 200
elif [ "$TEST_RESPONSE" = transport ]; then
    exit 7
elif [ "$TEST_RESPONSE" = html ]; then
    printf '<html>Bad gateway</html>' > "$output"
    printf 502
else
    jq -n --arg code "$TEST_RESPONSE" '{error:{code:$code,message:"test refusal"}}' > "$output"
    printf 503
fi
CURL
chmod +x "$task_tmp/bin/curl"
export PATH="$task_tmp/bin:$PATH"
export ARRAKIS_SIGNAL_DATE=2026-09-08 ARRAKIS_SIGNAL_OUT_DIR="$task_tmp/output" ARRAKIS_SIGNAL_SYMBOLS=XLF
for script in export_daily_signal.sh export_market_signals.sh; do
    if [ "$script" = export_daily_signal.sh ]; then file="$task_tmp/output/latest.json"; else file="$task_tmp/output/XLF-latest.json"; fi
    TEST_RESPONSE=NO_VALIDATED_MODEL bash "$backend_root/scripts/$script"
    jq -e '.prediction == null and .prediction_error.code == "NO_VALIDATED_MODEL" and ([.articles[] | has("body")] | any | not)' "$file" >/dev/null
    cp "$file" "$task_tmp/preserved.json"
    for failure in MODEL_UNAVAILABLE ML_DATABASE_UNAVAILABLE html transport; do
        if TEST_RESPONSE=$failure bash "$backend_root/scripts/$script" >/dev/null 2>&1; then
            echo "FAIL: $script accepted $failure" >&2; exit 1
        fi
        cmp "$task_tmp/preserved.json" "$file"
    done
done
echo 'PASS: validation refusal is publishable; provider, model and transport failures preserve the last good result'
