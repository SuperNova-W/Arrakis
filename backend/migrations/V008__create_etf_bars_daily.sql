CREATE TABLE IF NOT EXISTS etf_bars_daily (
    symbol TEXT NOT NULL REFERENCES etf_metadata(symbol),
    trading_date DATE NOT NULL,
    open DOUBLE PRECISION NOT NULL CHECK (open > 0),
    high DOUBLE PRECISION NOT NULL CHECK (high > 0),
    low DOUBLE PRECISION NOT NULL CHECK (low > 0),
    close DOUBLE PRECISION NOT NULL CHECK (close > 0),
    volume DOUBLE PRECISION NOT NULL CHECK (volume >= 0),
    source TEXT NOT NULL,
    inserted_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (symbol, trading_date),
    CHECK (low <= open AND open <= high),
    CHECK (low <= close AND close <= high)
);

CREATE INDEX IF NOT EXISTS etf_bars_daily_date_desc ON etf_bars_daily (trading_date DESC, symbol);

ALTER TABLE etf_bars_daily ENABLE ROW LEVEL SECURITY;

DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'anon') THEN
    REVOKE ALL ON TABLE etf_bars_daily FROM anon;
  END IF;
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'authenticated') THEN
    REVOKE ALL ON TABLE etf_bars_daily FROM authenticated;
  END IF;
END $$;
