ALTER TABLE etf_metadata ENABLE ROW LEVEL SECURITY;
ALTER TABLE etf_bars_1m ENABLE ROW LEVEL SECURITY;
ALTER TABLE etf_bars_5m ENABLE ROW LEVEL SECURITY;

DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'anon') THEN
    REVOKE ALL ON TABLE etf_metadata, etf_bars_1m, etf_bars_5m FROM anon;
  END IF;
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'authenticated') THEN
    REVOKE ALL ON TABLE etf_metadata, etf_bars_1m, etf_bars_5m FROM authenticated;
  END IF;
END $$;
