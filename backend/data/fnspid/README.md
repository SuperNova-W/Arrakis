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

After downloading the upstream file to `data/fnspid/raw/nasdaq_exteral_data.csv`:

```sh
./build/local/services/ml_model/arrakis-import-fnspid \
  --input data/fnspid/raw/nasdaq_exteral_data.csv \
  --holdings data/metadata/xlk_holdings_history.csv \
  --output data/fnspid/normalized/xlk_articles.csv \
  --manifest data/fnspid/manifests/import.json \
  --from 2016-01-01 --to 2023-12-31
```

The output is normalized, deduplicated, and filtered to symbols that were XLK
constituents on the article date. `manifest.json` records skips and duplicates.
