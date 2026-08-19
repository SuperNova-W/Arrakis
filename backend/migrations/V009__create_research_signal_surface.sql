-- Durable research-signal storage and the minimal read-only surface the browser
-- is allowed to see.
--
-- V006, V007 and V008 enable row-level security and REVOKE ALL from `anon` and
-- `authenticated` on every table, which is correct: none of those tables should
-- ever be world-readable. The frontend now reads Supabase directly instead of
-- calling an always-on market-api, so it needs exactly one published surface --
-- not access to the base tables.
--
-- The pattern below keeps the base table revoked and grants SELECT only on two
-- views. A view in PostgreSQL 15+ defaults to security_invoker = false, so it
-- executes with the view owner's privileges and the anon role never touches
-- research_signals directly.

CREATE TABLE IF NOT EXISTS research_signals (
    symbol TEXT NOT NULL REFERENCES etf_metadata(symbol),
    trading_date DATE NOT NULL,
    generated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    -- 'post_close' is the signal of record: its feature window matches the
    -- training distribution, which always saw a full day of news up to the
    -- 16:00 America/New_York close. 'intraday' runs are point-in-time correct
    -- but out-of-distribution, and must be labelled as provisional wherever
    -- they are displayed.
    run_kind TEXT NOT NULL CHECK (run_kind IN ('intraday', 'post_close')),
    cutoff_timestamp TIMESTAMPTZ NOT NULL,
    window_start_timestamp TIMESTAMPTZ NOT NULL,
    coverage_status TEXT NOT NULL CHECK (coverage_status IN ('complete', 'partial', 'empty', 'stale')),
    article_count INTEGER NOT NULL DEFAULT 0 CHECK (article_count >= 0),
    -- FALSE whenever the model gate is closed. A document with
    -- model_validated = FALSE carries no prediction, only a prediction_error.
    model_validated BOOLEAN NOT NULL DEFAULT FALSE,
    feature_schema_hash TEXT NOT NULL,
    source_commit TEXT,
    pipeline_run TEXT,
    -- The published research document, already stripped of article bodies.
    document JSONB NOT NULL,
    PRIMARY KEY (symbol, trading_date, generated_at)
);

CREATE INDEX IF NOT EXISTS research_signals_recent_idx
    ON research_signals (symbol, trading_date DESC, generated_at DESC);

ALTER TABLE research_signals ENABLE ROW LEVEL SECURITY;

-- The most recent run per symbol and trading date, newest date first. This is
-- what the dashboard renders.
CREATE OR REPLACE VIEW public_research_signal_latest AS
SELECT DISTINCT ON (symbol, trading_date)
       symbol,
       trading_date,
       generated_at,
       run_kind,
       cutoff_timestamp,
       coverage_status,
       article_count,
       model_validated,
       feature_schema_hash,
       source_commit,
       pipeline_run,
       document
FROM research_signals
ORDER BY symbol, trading_date DESC, generated_at DESC;

-- Every run for a date, so the page can show how the provisional intraday
-- reading evolved toward the post-close signal of record.
CREATE OR REPLACE VIEW public_research_signal_runs AS
SELECT symbol,
       trading_date,
       generated_at,
       run_kind,
       cutoff_timestamp,
       coverage_status,
       article_count,
       model_validated
FROM research_signals;

DO $$
BEGIN
  -- Grant the published views only. research_signals itself stays revoked, and
  -- no other table becomes reachable.
  -- Supabase's project-level default privileges auto-grant ALL on newly
  -- created relations to anon/authenticated, so CREATE OR REPLACE VIEW above
  -- may already have handed out INSERT/UPDATE/DELETE/TRUNCATE. GRANT SELECT
  -- alone only adds to that; it does not strip it. REVOKE ALL first, on the
  -- views too, so the two views end up SELECT-only exactly like the table.
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'anon') THEN
    REVOKE ALL ON TABLE research_signals FROM anon;
    REVOKE ALL ON public_research_signal_latest FROM anon;
    REVOKE ALL ON public_research_signal_runs FROM anon;
    GRANT SELECT ON public_research_signal_latest TO anon;
    GRANT SELECT ON public_research_signal_runs TO anon;
  END IF;
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'authenticated') THEN
    REVOKE ALL ON TABLE research_signals FROM authenticated;
    REVOKE ALL ON public_research_signal_latest FROM authenticated;
    REVOKE ALL ON public_research_signal_runs FROM authenticated;
    GRANT SELECT ON public_research_signal_latest TO authenticated;
    GRANT SELECT ON public_research_signal_runs TO authenticated;
  END IF;
END $$;

-- Retention. The free Supabase tier caps the database at 500 MB, and
-- news_nlp_features.pooled_embedding stores a full FinBERT hidden vector as
-- JSONB per article, which dominates growth. This function is called by the
-- pipeline after each run; it is deliberately a function rather than a trigger
-- so the retention window is visible and adjustable in one place.
CREATE OR REPLACE FUNCTION prune_research_history(retain_days INTEGER DEFAULT 120)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    horizon DATE := (NOW() AT TIME ZONE 'UTC')::date - retain_days;
BEGIN
    DELETE FROM research_signals WHERE trading_date < horizon;
    -- news_article_entities and news_nlp_features cascade from news_articles.
    DELETE FROM news_articles WHERE published_at < horizon;
    DELETE FROM etf_daily_news_features WHERE trading_date < horizon;
END;
$$;
