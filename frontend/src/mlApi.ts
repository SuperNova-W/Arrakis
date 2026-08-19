import { useEffect, useState } from 'react'

// The XLK research document is produced by the hourly GitHub Actions pipeline
// (.github/workflows/hourly-signal.yml) and stored in Supabase. The browser
// reads it back directly through the `public_research_signal_latest` view using
// the anon key -- there is no always-on backend, and the whole deployment costs
// $0/month. See backend/docs/zero-cost-deployment.md.
//
// Plain fetch against PostgREST rather than @supabase/supabase-js: this is a
// single filtered GET against one read-only view, and the client library would
// add a dependency (and bundle weight) for auth, realtime and storage features
// that are not used here. The REST shape below is stable PostgREST.
//
// The anon key is a public, publishable credential -- it is safe in the bundle
// precisely because the surface it can reach is exactly the two views granted
// in migrations/V009__create_research_signal_surface.sql, and nothing else.
const SUPABASE_URL = (import.meta.env.VITE_SUPABASE_URL ?? '').trim().replace(/\/$/, '')
const SUPABASE_ANON_KEY = (import.meta.env.VITE_SUPABASE_ANON_KEY ?? '').trim()

export type MlErrorCode = 'MODEL_UNAVAILABLE' | 'NO_VALIDATED_MODEL' | 'FEATURE_SCHEMA_MISMATCH' | 'ML_DATABASE_UNAVAILABLE' | 'FEATURES_UNAVAILABLE' | 'INVALID_DATE' | 'NOT_CONFIGURED' | 'NETWORK_ERROR' | 'UNKNOWN'

export class MlApiError extends Error {
  readonly code: MlErrorCode
  readonly status?: number

  constructor(message: string, code: MlErrorCode, status?: number) {
    super(message)
    this.name = 'MlApiError'
    this.code = code
    this.status = status
  }
}

export interface MlPrediction {
  direction: 'Bullish' | 'Neutral' | 'Bearish' | string
  probability_positive_return: number
  threshold: number
  model_id: string
}

export interface MlArticle {
  article_id: string
  source: string
  headline: string
  url?: string
  published_at: string
  sentiment_score: number
  positive_probability: number
  negative_probability: number
}

/** Why no prediction is present. Absent when a validated model produced one. */
export interface MlPredictionError {
  code: string
  message: string
  http_status: number
}

export interface MlPayload {
  symbol: string
  date: string
  coverage_status: 'complete' | 'partial' | 'empty' | 'stale' | string
  feature_schema_hash: string
  publication_cutoff?: string
  prediction?: MlPrediction
  prediction_error?: MlPredictionError | null
  dominant_themes?: string[]
  articles: MlArticle[]
  research_only_disclaimer: string
  // Provenance stamped by the publishing pipeline.
  generated_at?: string
  source_commit?: string
  model_validated?: boolean
  pipeline_run?: string
  /**
   * 'post_close' is the signal of record: its news window covers the full
   * trading day, which is the distribution every training row was drawn from.
   * 'intraday' runs are point-in-time correct but see only part of the day's
   * news, so their features sit outside that distribution. The UI must label
   * them as provisional rather than presenting them as equivalent evidence.
   */
  run_kind?: 'intraday' | 'post_close' | string
  signal_of_record?: boolean
  window_start?: string | null
}

interface EndpointState {
  data: MlPayload | null
  error: MlApiError | null
}

interface MlState {
  loading: boolean
  prediction: EndpointState
  news: EndpointState
  insights: EndpointState
  refresh: () => void
}

const SUPPORTED_CODES: MlErrorCode[] = ['MODEL_UNAVAILABLE', 'NO_VALIDATED_MODEL', 'FEATURE_SCHEMA_MISMATCH', 'ML_DATABASE_UNAVAILABLE', 'FEATURES_UNAVAILABLE', 'INVALID_DATE']

function asErrorCode(code: string | undefined): MlErrorCode {
  return SUPPORTED_CODES.includes(code as MlErrorCode) ? code as MlErrorCode : 'UNKNOWN'
}

/** One row of `public_research_signal_latest`. */
interface SignalRow {
  trading_date: string
  generated_at: string
  run_kind: string
  model_validated: boolean
  document: MlPayload
}

async function fetchDocument(date: string, signal: AbortSignal): Promise<MlPayload> {
  if (!/^\d{4}-\d{2}-\d{2}$/.test(date)) {
    throw new MlApiError('Date must be formatted YYYY-MM-DD.', 'INVALID_DATE')
  }
  if (!SUPABASE_URL || !SUPABASE_ANON_KEY) {
    throw new MlApiError(
      'VITE_SUPABASE_URL and VITE_SUPABASE_ANON_KEY are not configured for this build.',
      'NOT_CONFIGURED',
    )
  }

  const query = new URLSearchParams({
    symbol: 'eq.XLK',
    trading_date: `eq.${date}`,
    select: 'trading_date,generated_at,run_kind,model_validated,document',
    limit: '1',
  })
  const response = await fetch(`${SUPABASE_URL}/rest/v1/public_research_signal_latest?${query}`, {
    signal,
    headers: {
      apikey: SUPABASE_ANON_KEY,
      Authorization: `Bearer ${SUPABASE_ANON_KEY}`,
      Accept: 'application/json',
    },
    cache: 'no-cache',
  })

  if (!response.ok) {
    throw new MlApiError(
      `The research store returned HTTP ${response.status}.`,
      response.status === 401 || response.status === 403 ? 'ML_DATABASE_UNAVAILABLE' : 'UNKNOWN',
      response.status,
    )
  }

  const rows = await response.json().catch(() => null) as SignalRow[] | null
  const row = Array.isArray(rows) ? rows[0] : undefined
  if (!row) {
    throw new MlApiError(
      `No research document has been published for ${date}. Documents are generated on trading days only.`,
      'FEATURES_UNAVAILABLE',
      404,
    )
  }

  const document = row.document
  if (!document || typeof document.date !== 'string') {
    throw new MlApiError(`The research document for ${date} is malformed.`, 'FEATURES_UNAVAILABLE', response.status)
  }
  // Trust the indexed columns over the embedded copy: they are what the
  // pipeline wrote and what the view ordered by.
  return {
    ...document,
    articles: document.articles ?? [],
    generated_at: row.generated_at ?? document.generated_at,
    run_kind: row.run_kind ?? document.run_kind,
    signal_of_record: (row.run_kind ?? document.run_kind) === 'post_close',
    model_validated: row.model_validated ?? document.model_validated,
  }
}

function toError(error: unknown): MlApiError {
  if (error instanceof MlApiError) return error
  if (error instanceof DOMException && error.name === 'AbortError') return new MlApiError('The request was cancelled.', 'NETWORK_ERROR')
  return new MlApiError(error instanceof Error ? error.message : 'The research document could not be loaded.', 'NETWORK_ERROR')
}

/**
 * Loads the most recent published research document for `date` and projects it
 * onto the three slots the UI expects. The prediction slot carries an error
 * whenever the document has no prediction -- which is the normal state while the
 * model gate is closed, and is what drives the "No validated model" panel.
 */
export function useMlRecommendation(date: string): MlState {
  const [refreshKey, setRefreshKey] = useState(0)
  const [state, setState] = useState<Omit<MlState, 'refresh'>>({
    loading: true,
    prediction: { data: null, error: null },
    news: { data: null, error: null },
    insights: { data: null, error: null },
  })

  useEffect(() => {
    const controller = new AbortController()
    let cancelled = false

    void (async () => {
      try {
        const document = await fetchDocument(date, controller.signal)
        if (cancelled || controller.signal.aborted) return
        const populated: EndpointState = { data: document, error: null }
        const prediction: EndpointState = document.prediction
          ? populated
          : {
              data: document,
              error: new MlApiError(
                document.prediction_error?.message
                  ?? 'No prediction is available for this date.',
                asErrorCode(document.prediction_error?.code),
                document.prediction_error?.http_status,
              ),
            }
        setState({ loading: false, prediction, news: populated, insights: populated })
      } catch (error) {
        if (cancelled || controller.signal.aborted) return
        const failure: EndpointState = { data: null, error: toError(error) }
        setState({ loading: false, prediction: failure, news: failure, insights: failure })
      }
    })()

    return () => { cancelled = true; controller.abort() }
  }, [date, refreshKey])

  return { ...state, refresh: () => setRefreshKey(value => value + 1) }
}
