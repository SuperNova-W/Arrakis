#!/bin/sh
set -eu

binary=$1
backend_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fixture_root="$backend_root/tests/fixtures"
output=$(mktemp "${TMPDIR:-/tmp}/arrakis-mrn-test.XXXXXX.csv")
trap 'rm -f "$output" "$output.manifest.json"' EXIT

"$binary" \
    "$fixture_root/mrn_small.jsonl" \
    "$fixture_root/mrn_entities.csv" \
    "$fixture_root/mrn_holdings_small.csv" \
    "$backend_root/data/history/SPY.csv" \
    2019-01-02 2019-01-07 "$output" >/dev/null

test "$(wc -l < "$output" | tr -d ' ')" = 3
rg -q '^mrn:story-1:1:AAA,.*2019-01-02,XLB,AAA,Initial technology demand,' "$output"
rg -q '^mrn:story-2:2:AAA,.*2019-01-03,XLB,AAA,Same-session corrected update,' "$output"
if rg -q 'Updated technology demand|After-close technology update' "$output"; then
    echo "MRN version selection retained a superseded row" >&2
    exit 1
fi
if rg -q 'Removed holding should be rejected' "$output"; then
    echo "MRN membership snapshot retained a removed symbol" >&2
    exit 1
fi
rg -q '"withdrawals_applied": 1' "$output.manifest.json"
rg -q '"unknown_entity_references": 1' "$output.manifest.json"
rg -q '"membership_rejects": 1' "$output.manifest.json"
echo "PASS: version-aware MRN cutoff, correction, withdrawal, and membership fixture"
