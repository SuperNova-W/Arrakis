#!/bin/sh
set -eu

# Extracts the XLK series from SEC DERA N-PORT ZIPs. This is a data-prep
# utility, not a runtime dependency. It intentionally carries each reported
# snapshot forward only until the next reported date; it does not claim daily
# holdings precision between SEC reporting dates.

input_dir=${1:?usage: extract_xlk_holdings.sh <sec_nport_dir> <output_csv> <ticker_json>}
output_csv=${2:?usage: extract_xlk_holdings.sh <sec_nport_dir> <output_csv> <ticker_json>}
ticker_json=${3:?usage: extract_xlk_holdings.sh <sec_nport_dir> <output_csv> <ticker_json>}

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT INT TERM

# SEC's company reference is used only to resolve issuer names to symbols. ETF
# membership itself comes exclusively from the SEC XLK series snapshots.
jq -r '.data[] | [(.[1] | ascii_upcase), .[2]] | @tsv' "$ticker_json" > "$work_dir/names.tsv"

printf 'symbol,effective_from,effective_to,weight,source\n' > "$output_csv"
printf 'quarter,report_date,accession,issuer_rows,unresolved_rows\n' > "${output_csv}.manifest.csv"

for archive in "$input_dir"/*_nport.zip; do
    quarter=$(basename "$archive" _nport.zip)
    unzip -p "$archive" FUND_REPORTED_INFO.tsv > "$work_dir/fund_info.tsv"
    unzip -p "$archive" SUBMISSION.tsv > "$work_dir/submission.tsv"
    unzip -p "$archive" FUND_REPORTED_HOLDING.tsv > "$work_dir/holding.tsv"

    awk -F '\t' '$2 == "The Technology Select Sector SPDR Fund" {print $1}' \
        "$work_dir/fund_info.tsv" > "$work_dir/accessions.tsv"
    if [ ! -s "$work_dir/accessions.tsv" ]; then
        continue
    fi

    while IFS= read -r accession; do
        report_date=$(awk -F '\t' -v a="$accession" '$1 == a {print $6; exit}' "$work_dir/submission.tsv")
        [ -n "$report_date" ] || continue
        date=$(printf '%s' "$report_date" | tr '[:upper:]' '[:lower:]' | awk -F '-' '{printf "%s-%02d-%02d", $3, (index("janfebmaraprmayjunjulaugsepoctnovdec", substr($2,1,3))+2)/3, $1}')
        awk -F '\t' -v a="$accession" '$1 == a && $13 != "" {print $3 "\t" $13}' "$work_dir/holding.tsv" > "$work_dir/current.tsv"
        total=$(wc -l < "$work_dir/current.tsv" | tr -d ' ')
        unresolved_file="$work_dir/unresolved_${quarter}.txt"
        awk -F '\t' -v d="$date" -v q="$quarter" -v unresolved_file="$unresolved_file" '
            NR == FNR { x=$1; gsub(/[^A-Z0-9]/, "", x); ticker[x]=$2; next }
            { x=toupper($1); gsub(/[^A-Z0-9]/, "", x); if (x in ticker) print ticker[x] "," d ",2099-12-31," $2 ",SEC-NPORT-" q; else unresolved++ }
            END { print unresolved + 0 > unresolved_file }
        ' "$work_dir/names.tsv" "$work_dir/current.tsv" >> "$output_csv"
        unresolved=$(sed -n '1p' "$unresolved_file")
        printf '%s,%s,%s,%s,%s\n' "$quarter" "$date" "$accession" "$total" "$unresolved" >> "${output_csv}.manifest.csv"
    done < "$work_dir/accessions.tsv"
done

# Collapse duplicate issuer snapshots, retain the latest row for each symbol
# and report date. Interval expansion is performed by the downstream builder.
sort -t, -k1,1 -k2,2 -k5,5 -u "$output_csv" > "$work_dir/sorted.csv"
mv "$work_dir/sorted.csv" "$output_csv"
