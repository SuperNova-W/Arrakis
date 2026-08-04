import { useCallback, useEffect, useState } from 'react'
import type { Candle, ChartRange } from '../finnhub/types'
import { getCandles, TwelveDataError } from './client'

type ResourceState = {
  key: string
  data: Candle[] | null
  loading: boolean
  stale: boolean
  cached: boolean
  error: TwelveDataError | null
}

export function useTwelveDataCandles(symbol: string, range: ChartRange, apiKey: string) {
  const resourceKey = `${symbol}:${range}:${apiKey ? 'key' : 'none'}`
  const enabled = Boolean(symbol && apiKey)
  const [refreshKey, setRefreshKey] = useState(0)
  const [state, setState] = useState<ResourceState>({
    key: resourceKey,
    data: null,
    loading: enabled,
    stale: false,
    cached: false,
    error: null,
  })

  const loader = useCallback((signal: AbortSignal) => getCandles(symbol, range, apiKey, signal), [symbol, range, apiKey])

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
          error: error instanceof TwelveDataError
            ? error
            : new TwelveDataError(error instanceof Error ? error.message : 'Twelve Data request failed.', 'NETWORK'),
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
