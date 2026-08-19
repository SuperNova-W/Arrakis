# SEC Form N-PORT source

These local ZIP files are downloaded from the SEC DERA Form N-PORT bulk data
sets. They are not committed to git because they are large. Each quarterly
dataset contains `FUND_REPORTED_INFO.tsv`, `FUND_REPORTED_HOLDING.tsv`,
`IDENTIFIERS.tsv`, and `SUBMISSION.tsv`.

The configured sector series are identified by these SEC `SERIES_NAME`/`SERIES_ID` pairs:

```text
The Communication Services Select Sector SPDR Fund  S000062095  XLC
The Consumer Discretionary Select Sector SPDR Fund  S000006408  XLY
The Consumer Staples Select Sector SPDR Fund         S000006409  XLP
The Energy Select Sector SPDR Fund                   S000006410  XLE
The Financial Select Sector SPDR Fund                S000006411  XLF
The Health Care Select Sector SPDR Fund              S000006412  XLV
The Industrial Select Sector SPDR Fund               S000006413  XLI
The Materials Select Sector SPDR Fund                S000006414  XLB
The Real Estate Select Sector SPDR Fund              S000051152  XLRE
The Technology Select Sector SPDR Fund               S000006415  XLK
The Utilities Select Sector SPDR Fund                S000006416  XLU
```

The portfolio reporting date comes from `SUBMISSION.REPORT_DATE`, while the
point-in-time availability date comes from `SUBMISSION.FILING_DATE`. Holdings
are joined by `ACCESSION_NUMBER` and `HOLDING_ID`. The multi-sector extractor
uses a snapshot only for article dates strictly after its filing date; this is a
conservative date-only approximation of SEC acceptance timing and avoids using
membership before the filing was public. It uses the official SEC company-ticker
reference only to resolve issuer names to symbols; it does not substitute
current holdings for a missing snapshot.

To rebuild all sector snapshots:

```sh
./data/metadata/sec_nport/extract_sector_holdings.sh \
  data/metadata/sec_nport \
  /private/tmp/arrakis_sector_holdings_history.csv \
  /private/tmp/arrakis_company_tickers.json
```
