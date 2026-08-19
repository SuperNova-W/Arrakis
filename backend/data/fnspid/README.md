# FNSPID XLK research data

The raw FNSPID CSV is intentionally not checked into git. The upstream file is
approximately 23 GB and is licensed CC BY-NC 4.0; use it only for permitted
non-commercial research.

Before importing, create `backend/data/metadata/xlk_holdings_history.csv` from
an auditable historical constituent source. It must contain:

```text
symbol,effective_from,effective_to,weight,source
```

The importer fails if this file is missing. It never substitutes current XLK
holdings for historical dates, because that would introduce survivorship bias.
Each SEC N-PORT filing date is treated as a complete point-in-time snapshot.
The latest snapshot strictly at or before the assigned prediction session
fully supersedes the prior snapshot, including for symbols that leave XLK;
there is no current-holdings fallback. The applied policy is recorded in the
import manifest.

After downloading the upstream file to `data/fnspid/raw/nasdaq_exteral_data.csv`:

```sh
./build/local/services/ml_model/arrakis-import-fnspid \
  --input data/fnspid/raw/nasdaq_exteral_data.csv \
  --holdings data/metadata/xlk_holdings_history.csv \
  --market-history data/history/SPY.csv \
  --output data/fnspid/normalized/xlk_articles.csv \
  --manifest data/fnspid/manifests/import.json \
  --provenance data/fnspid/manifests/import_provenance.csv \
  --from 2016-01-01 --to 2023-12-31
```

The output is normalized, deduplicated, and filtered to symbols that were XLK
constituents using the latest holdings snapshot strictly before the assigned
prediction session. `trading_date` is the first
market session whose 09:20 ET publication cutoff is at or after the source UTC
timestamp; this handles after-hours and weekend articles without silently
dropping them or attaching them to the wrong session. `manifest.json` records
the session-assignment policy, skips, and duplicates. The provenance CSV records
the source timestamp, assigned session, cutoff, governing holdings snapshot,
and membership extrapolation flag for every retained row. A snapshot dated on
the event session is deliberately not used, because its filing/publication
time is not available in this date-only holdings file.

For combined FinBERT features, the assigned event session is joined to the
preceding session's market row: market features use information through close
`t`, news is eligible through the next session's 09:20 ET cutoff, and the label
is `close[t+1] > close[t]`. Pass a cache directory to
`arrakis-build-xlk-combined`; its exact token-input hashes and pooled outputs
are flushed per batch so a GPU interruption can resume without recomputing
completed inference.
