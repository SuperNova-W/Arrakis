import { useCallback, useEffect, useState } from 'react'
import { getCandles, getEtfProfile, getProfile, getQuote, FinnhubError } from './client'
import type { Candle, ChartRange, EtfProfile, FinnhubProfile, IndicatorBundle, Quote } from './types'

type ResourceState<T> = {
  key: string
  data: T | null
  loading: boolean
  stale: boolean
  cached: boolean
  error: FinnhubError | null
}

function useFinnhubResource<T>(
  resourceKey: string,
  enabled: boolean,
  loader: (signal: AbortSignal) => Promise<{ value: T; stale: boolean; cached: boolean }>,
) {
  const [refreshKey, setRefreshKey] = useState(0)
  const [state, setState] = useState<ResourceState<T>>({
    key: resourceKey,
    data: null,
    loading: enabled,
    stale: false,
    cached: false,
    error: null,
  })

  useEffect(() => {
    if (!enabled) return
    const controller = new AbortController()
    loader(controller.signal)
      .then(result => setState({ key: resourceKey, data: result.value, loading: false, stale: result.stale, cached: result.cached, error: null }))
      .catch(error => {
        if (controller.signal.aborted) return
        setState(current => ({
          ...current,
          key: resourceKey,
          loading: false,
          error: error instanceof FinnhubError
            ? error
            : new FinnhubError(error instanceof Error ? error.message : 'Finnhub request failed.', 'NETWORK'),
        }))
      })
    return () => controller.abort()
  }, [resourceKey, enabled, refreshKey, loader])

  const current = state.key === resourceKey
  return {
    ...state,
    data: current ? state.data : null,
    loading: enabled && (!current || state.loading),
    error: current ? state.error : null,
    refresh: () => setRefreshKey(value => value + 1),
  }
}

export function useFinnhubQuote(symbol: string, apiKey: string) {
  const loader = useCallback((signal: AbortSignal) => getQuote(symbol, apiKey, signal), [symbol, apiKey])
  return useFinnhubResource<Quote>(`quote:${symbol}:${apiKey ? 'key' : 'none'}`, Boolean(symbol && apiKey), loader)
}

export function useFinnhubCandles(symbol: string, range: ChartRange, apiKey: string) {
  const loader = useCallback((signal: AbortSignal) => getCandles(symbol, range, apiKey, signal), [symbol, range, apiKey])
  return useFinnhubResource<Candle[]>(`candles:${symbol}:${range}:${apiKey ? 'key' : 'none'}`, Boolean(symbol && apiKey), loader)
}

export function useFinnhubProfile(symbol: string, apiKey: string) {
  const loader = useCallback((signal: AbortSignal) => getProfile(symbol, apiKey, signal), [symbol, apiKey])
  return useFinnhubResource<FinnhubProfile>(`profile:${symbol}:${apiKey ? 'key' : 'none'}`, Boolean(symbol && apiKey), loader)
}

export function useFinnhubEtfProfile(symbol: string, apiKey: string) {
  const loader = useCallback((signal: AbortSignal) => getEtfProfile(symbol, apiKey, signal), [symbol, apiKey])
  return useFinnhubResource<EtfProfile>(`etf-profile:${symbol}:${apiKey ? 'key' : 'none'}`, Boolean(symbol && apiKey), loader)
}

export function useIndicators(candles: Candle[]) {
  const [indicators, setIndicators] = useState<IndicatorBundle | null>(null)
  useEffect(() => {
    if (!candles.length) return
    const worker = new Worker(new URL('./indicator.worker.ts', import.meta.url), { type: 'module' })
    worker.onmessage = (event: MessageEvent<IndicatorBundle>) => setIndicators(event.data)
    worker.postMessage(candles)
    return () => worker.terminate()
  }, [candles])
  return candles.length ? indicators : null
}
