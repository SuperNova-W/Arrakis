CREATE INDEX IF NOT EXISTS etf_bars_1m_symbol_start_desc ON etf_bars_1m (symbol, bar_start DESC);
CREATE INDEX IF NOT EXISTS etf_bars_1m_start_desc ON etf_bars_1m (bar_start DESC);
CREATE INDEX IF NOT EXISTS etf_bars_5m_symbol_start_desc ON etf_bars_5m (symbol, bar_start DESC);
CREATE INDEX IF NOT EXISTS etf_bars_5m_start_desc ON etf_bars_5m (bar_start DESC);
