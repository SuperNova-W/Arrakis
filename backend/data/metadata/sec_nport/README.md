# SEC Form N-PORT source

These local ZIP files are downloaded from the SEC DERA Form N-PORT bulk data
sets. They are not committed to git because they are large. Each quarterly
dataset contains `FUND_REPORTED_INFO.tsv`, `FUND_REPORTED_HOLDING.tsv`,
`IDENTIFIERS.tsv`, and `SUBMISSION.tsv`.

The XLK series is identified by:

```text
SERIES_NAME: The Technology Select Sector SPDR Fund
SERIES_ID:   S000006415
```

The reporting date comes from `SUBMISSION.REPORT_DATE`, not the filing date.
Holdings are joined by `ACCESSION_NUMBER` and `HOLDING_ID`. The SEC data is
reported periodically, so the resulting mapping must preserve reporting dates
and document the interval policy used to apply a snapshot to trading dates.
