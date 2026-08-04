CREATE TABLE IF NOT EXISTS news_articles (
    article_id TEXT PRIMARY KEY,
    canonical_url TEXT NOT NULL,
    normalized_content_hash TEXT NOT NULL,
    source_id TEXT NOT NULL,
    headline TEXT NOT NULL,
    body TEXT NOT NULL DEFAULT '',
    published_at TIMESTAMPTZ NOT NULL,
    retrieved_at TIMESTAMPTZ NOT NULL,
    language TEXT NOT NULL DEFAULT 'en',
    duplicate_of TEXT REFERENCES news_articles(article_id),
    novelty_score DOUBLE PRECISION NOT NULL DEFAULT 1 CHECK (novelty_score >= 0 AND novelty_score <= 1),
    provenance JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE (canonical_url),
    UNIQUE (normalized_content_hash)
);

CREATE TABLE IF NOT EXISTS news_article_entities (
    article_id TEXT NOT NULL REFERENCES news_articles(article_id) ON DELETE CASCADE,
    entity_type TEXT NOT NULL CHECK (entity_type IN ('etf', 'company', 'sector', 'theme', 'macro')),
    entity_id TEXT NOT NULL,
    relevance DOUBLE PRECISION NOT NULL DEFAULT 1 CHECK (relevance >= 0 AND relevance <= 1),
    PRIMARY KEY (article_id, entity_type, entity_id)
);

CREATE TABLE IF NOT EXISTS news_nlp_features (
    article_id TEXT PRIMARY KEY REFERENCES news_articles(article_id) ON DELETE CASCADE,
    model_version TEXT NOT NULL,
    tokenizer_version TEXT NOT NULL,
    positive_probability DOUBLE PRECISION NOT NULL CHECK (positive_probability >= 0 AND positive_probability <= 1),
    neutral_probability DOUBLE PRECISION NOT NULL CHECK (neutral_probability >= 0 AND neutral_probability <= 1),
    negative_probability DOUBLE PRECISION NOT NULL CHECK (negative_probability >= 0 AND negative_probability <= 1),
    sentiment_score DOUBLE PRECISION NOT NULL,
    pooled_embedding JSONB NOT NULL DEFAULT '[]'::jsonb,
    feature_schema_hash TEXT NOT NULL,
    inference_latency_ms DOUBLE PRECISION NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    CHECK (ABS((positive_probability + neutral_probability + negative_probability) - 1) < 0.001)
);

CREATE TABLE IF NOT EXISTS etf_daily_news_features (
    symbol TEXT NOT NULL REFERENCES etf_metadata(symbol),
    trading_date DATE NOT NULL,
    cutoff_timestamp TIMESTAMPTZ NOT NULL,
    latest_eligible_article_at TIMESTAMPTZ,
    feature_schema_hash TEXT NOT NULL,
    features JSONB NOT NULL,
    article_count INTEGER NOT NULL DEFAULT 0 CHECK (article_count >= 0),
    coverage_status TEXT NOT NULL CHECK (coverage_status IN ('complete', 'partial', 'empty', 'stale')),
    missing_source_warnings JSONB NOT NULL DEFAULT '[]'::jsonb,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (symbol, trading_date)
);

CREATE TABLE IF NOT EXISTS model_registry (
    model_id TEXT PRIMARY KEY,
    model_type TEXT NOT NULL CHECK (model_type IN ('finbert_onnx', 'xgboost')),
    version TEXT NOT NULL,
    artifact_path TEXT NOT NULL,
    feature_schema_hash TEXT NOT NULL,
    metadata JSONB NOT NULL DEFAULT '{}'::jsonb,
    active BOOLEAN NOT NULL DEFAULT FALSE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE (model_type, version)
);

CREATE INDEX IF NOT EXISTS news_articles_published_idx ON news_articles (published_at DESC);
CREATE INDEX IF NOT EXISTS news_articles_source_idx ON news_articles (source_id, published_at DESC);
CREATE INDEX IF NOT EXISTS news_entities_entity_idx ON news_article_entities (entity_type, entity_id, article_id);
CREATE INDEX IF NOT EXISTS daily_news_features_date_idx ON etf_daily_news_features (trading_date DESC, symbol);

ALTER TABLE news_articles ENABLE ROW LEVEL SECURITY;
ALTER TABLE news_article_entities ENABLE ROW LEVEL SECURITY;
ALTER TABLE news_nlp_features ENABLE ROW LEVEL SECURITY;
ALTER TABLE etf_daily_news_features ENABLE ROW LEVEL SECURITY;
ALTER TABLE model_registry ENABLE ROW LEVEL SECURITY;

DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'anon') THEN
    REVOKE ALL ON TABLE news_articles, news_article_entities, news_nlp_features, etf_daily_news_features, model_registry FROM anon;
  END IF;
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'authenticated') THEN
    REVOKE ALL ON TABLE news_articles, news_article_entities, news_nlp_features, etf_daily_news_features, model_registry FROM authenticated;
  END IF;
END $$;
