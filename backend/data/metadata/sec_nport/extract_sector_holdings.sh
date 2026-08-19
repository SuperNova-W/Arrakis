#!/bin/sh
set -eu

# Extracts point-in-time constituent snapshots for all 11 Select Sector SPDR
# funds from SEC DERA N-PORT bulk archives. The output is intentionally a
# snapshot table; downstream consumers apply snapshot k to dates from
# effective_from[k] through the day before effective_from[k+1].

input_dir=${1:?usage: extract_sector_holdings.sh <sec_nport_dir> <output_csv> <ticker_json>}
output_csv=${2:?usage: extract_sector_holdings.sh <sec_nport_dir> <output_csv> <ticker_json>}
ticker_json=${3:?usage: extract_sector_holdings.sh <sec_nport_dir> <output_csv> <ticker_json>}

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT INT TERM

# SEC company_tickers.json is an object keyed by row number; older SEC
# company_tickers_exchange.json files use a .data array. Accept both shapes.
jq -r 'if has("data") then .data[] | [(.[1] | ascii_upcase), .[2]] else .[] | [(.title | ascii_upcase), .ticker] end | @tsv' \
    "$ticker_json" | awk -F '\t' '!seen[$1]++' > "$work_dir/names.tsv"

printf 'sector,symbol,effective_from,available_from,effective_to,weight,source\n' > "$output_csv"
printf 'sector,quarter,report_date,available_from,accession,issuer_rows,unresolved_rows\n' > "${output_csv}.manifest.csv"

for archive in "$input_dir"/*_nport.zip; do
    quarter=$(basename "$archive" _nport.zip)
    unzip -p "$archive" FUND_REPORTED_INFO.tsv > "$work_dir/fund_info.tsv"
    unzip -p "$archive" SUBMISSION.tsv > "$work_dir/submission.tsv"
    unzip -p "$archive" FUND_REPORTED_HOLDING.tsv > "$work_dir/holding.tsv"

    awk -F '\t' '
        function sector(name, upper) {
            upper = toupper(name)
            if (upper ~ /COMMUNICATION SERVICES SELECT SECTOR SPDR/ && upper !~ /PREMIUM INCOME/) return "XLC"
            if (upper ~ /CONSUMER DISCRETIONARY SELECT SECTOR SPDR/ && upper !~ /PREMIUM INCOME/) return "XLY"
            if (upper ~ /CONSUMER STAPLES SELECT SECTOR SPDR/ && upper !~ /PREMIUM INCOME/) return "XLP"
            if (upper ~ /ENERGY SELECT SECTOR SPDR/ && upper !~ /PREMIUM INCOME/) return "XLE"
            if (upper ~ /FINANCIAL SELECT SECTOR SPDR/ && upper !~ /PREMIUM INCOME/) return "XLF"
            if (upper ~ /HEALTH CARE SELECT SECTOR SPDR/ && upper !~ /PREMIUM INCOME/) return "XLV"
            if (upper ~ /INDUSTRIAL SELECT SECTOR SPDR/ && upper !~ /PREMIUM INCOME/) return "XLI"
            if (upper ~ /MATERIALS SELECT SECTOR SPDR/ && upper !~ /PREMIUM INCOME/) return "XLB"
            if (upper ~ /REAL ESTATE SELECT SECTOR SPDR/ && upper !~ /PREMIUM INCOME/) return "XLRE"
            if (upper ~ /TECHNOLOGY SELECT SECTOR SPDR/ && upper !~ /PREMIUM INCOME/) return "XLK"
            if (upper ~ /UTILITIES SELECT SECTOR SPDR/ && upper !~ /PREMIUM INCOME/) return "XLU"
            return ""
        }
        {
            current = sector($2)
            if (current != "") print $1 "\t" current
        }
    ' "$work_dir/fund_info.tsv" > "$work_dir/funds.tsv"

    # Join the large holding table to the 11 selected accessions once. The
    # previous XLK-only utility could rescan this table for every fund without
    # consequence; doing that for the full sector universe is unnecessarily
    # expensive.
    awk -F '\t' '
        NR == FNR { sector[$1] = $2; next }
        $1 in sector && $13 != "" { print sector[$1] "\t" $1 "\t" $3 "\t" $13 }
    ' "$work_dir/funds.tsv" "$work_dir/holding.tsv" > "$work_dir/current_all.tsv"

    while IFS='	' read -r accession sector; do
        report_date=$(awk -F '\t' -v a="$accession" '$1 == a {print $6; exit}' "$work_dir/submission.tsv")
        filing_date=$(awk -F '\t' -v a="$accession" '$1 == a {print $2; exit}' "$work_dir/submission.tsv")
        [ -n "$report_date" ] || continue
        [ -n "$filing_date" ] || continue
        date=$(printf '%s' "$report_date" | tr '[:upper:]' '[:lower:]' | awk -F '-' '{printf "%s-%02d-%02d", $3, (index("janfebmaraprmayjunjulaugsepoctnovdec", substr($2,1,3))+2)/3, $1}')
        available_date=$(printf '%s' "$filing_date" | tr '[:upper:]' '[:lower:]' | awk -F '-' '{printf "%s-%02d-%02d", $3, (index("janfebmaraprmayjunjulaugsepoctnovdec", substr($2,1,3))+2)/3, $1}')
        awk -F '\t' -v s="$sector" -v a="$accession" '$1 == s && $2 == a {print $3 "\t" $4}' "$work_dir/current_all.tsv" > "$work_dir/current.tsv"
        total=$(wc -l < "$work_dir/current.tsv" | tr -d ' ')
        unresolved_file="$work_dir/unresolved_${quarter}_${sector}.txt"
        awk -F '\t' -v d="$date" -v a_date="$available_date" -v q="$quarter" -v s="$sector" -v unresolved_file="$unresolved_file" '
            function normalized(value) { value = toupper(value); gsub(/[^A-Z0-9]/, "", value); return value }
            NR == FNR { ticker[normalized($1)] = $2; next }
            {
                key = normalized($1)
                if (key in ticker) print s "," ticker[key] "," d "," a_date ",2099-12-31," $2 ",SEC-NPORT-" q
                else unresolved++
            }
            END { print unresolved + 0 > unresolved_file }
        ' "$work_dir/names.tsv" "$work_dir/current.tsv" >> "$output_csv"
        unresolved=$(sed -n '1p' "$unresolved_file")
        printf '%s,%s,%s,%s,%s,%s,%s\n' "$sector" "$quarter" "$date" "$available_date" "$accession" "$total" "$unresolved" >> "${output_csv}.manifest.csv"
    done < "$work_dir/funds.tsv"
done

# Keep deterministic ordering and remove exact duplicate issuer snapshots while
# retaining the schema header at the top of the generated file.
{
    head -n 1 "$output_csv"
    tail -n +2 "$output_csv" | sort -t, -k1,1 -k2,2 -k3,3 -k4,4 -k7,7 -u
} > "$work_dir/sorted.csv"
mv "$work_dir/sorted.csv" "$output_csv"
