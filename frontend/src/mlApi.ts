import { useEffect, useState } from 'react'

const MARKET_API_BASE = (import.meta.env.VITE_MARKET_API_URL ?? 'http://localhost:8080').replace(/\/$/, '')

export type MlErrorCode = 'MODEL_UNAVAILABLE' | 'FEATURE_SCHEMA_MISMATCH' | 'ML_DATABASE_UNAVAILABLE' | 'FEATURES_UNAVAILABLE' | 'INVALID_DATE' | 'NETWORK_ERROR' | 'UNKNOWN'

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
  canonical_url?: string
  published_at: string
  sentiment_score: number
  positive_probability: number
  negative_probability: number
}

export interface MlPayload {
  symbol: string
  date: string
  coverage_status: 'complete' | 'partial' | 'empty' | 'stale' | string
  feature_schema_hash: string
  prediction?: MlPrediction
  dominant_themes?: string[]
  articles: MlArticle[]
  research_only_disclaimer: string
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

async function request(date: string, endpoint: 'prediction' | 'news' | 'insights', signal: AbortSignal): Promise<MlPayload> {
  const response = await fetch(`${MARKET_API_BASE}/api/v1/etfs/XLK/${endpoint}?date=${encodeURIComponent(date)}`, { signal })
  const body = await response.json().catch(() => null) as { error?: { code?: string; message?: string } } | MlPayload | null
  if (!response.ok) {
    const error = body && 'error' in body ? body.error : undefined
    const code = error?.code
    const supported: MlErrorCode[] = ['MODEL_UNAVAILABLE', 'FEATURE_SCHEMA_MISMATCH', 'ML_DATABASE_UNAVAILABLE', 'FEATURES_UNAVAILABLE', 'INVALID_DATE']
    throw new MlApiError(error?.message ?? `Market API returned HTTP ${response.status}.`, supported.includes(code as MlErrorCode) ? code as MlErrorCode : 'UNKNOWN', response.status)
  }
  return body as MlPayload
}

function toError(error: unknown): MlApiError {
  if (error instanceof MlApiError) return error
  if (error instanceof DOMException && error.name === 'AbortError') return new MlApiError('The market API request was cancelled.', 'NETWORK_ERROR')
  return new MlApiError(error instanceof Error ? error.message : 'The market API could not be reached.', 'NETWORK_ERROR')
}

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
    const endpoints: Array<'prediction' | 'news' | 'insights'> = ['prediction', 'news', 'insights']
    void Promise.all(endpoints.map(async endpoint => {
      try {
        return { endpoint, result: { data: await request(date, endpoint, controller.signal), error: null } }
      } catch (error) {
        return { endpoint, result: { data: null, error: toError(error) } }
      }
    })).then(results => {
      if (controller.signal.aborted) return
      setState(current => {
        const next = { ...current }
        for (const item of results) next[item.endpoint] = item.result
        return { ...next, loading: false }
      })
    })
    return () => controller.abort()
  }, [date, refreshKey])

  return { ...state, refresh: () => setRefreshKey(value => value + 1) }
}
