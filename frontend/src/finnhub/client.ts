import { cacheIsFresh, readCache, writeCache } from './cache'
import { chartRangeRequest, isIntradayResolution } from './ranges'
import type { Candle, ChartRange, EtfHolding, EtfProfile, FinnhubProfile, Quote } from './types'

const BASE_URL = 'https://finnhub.io/api/v1'
const MAX_CONCURRENT_REQUESTS = 3

type QueueItem = {
  run: () => Promise<void>
  signal?: AbortSignal
}

const queue: QueueItem[] = []
let activeRequests = 0
const inFlight = new Map<string, Promise<unknown>>()

export class FinnhubError extends Error {
  constructor(
    message: string,
    readonly code: 'MISSING_KEY' | 'UNAUTHORIZED' | 'RATE_LIMITED' | 'NO_DATA' | 'ENTITLEMENT' | 'NETWORK' | 'INVALID_RESPONSE',
    readonly status = 0,
  ) {
    super(message)
    this.name = 'FinnhubError'
  }
}

function drainQueue() {
  while (activeRequests < MAX_CONCURRENT_REQUESTS && queue.length) {
    const item = queue.shift()
    if (!item || item.signal?.aborted) continue
    activeRequests += 1
    void item.run().finally(() => {
      activeRequests -= 1
      drainQueue()
    })
  }
}

function schedule<T>(operation: () => Promise<T>, signal?: AbortSignal): Promise<T> {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      reject(new DOMException('Request aborted', 'AbortError'))
      return
    }
    queue.push({
      signal,
      run: async () => {
        try {
          resolve(await operation())
        } catch (error) {
          reject(error)
        }
      },
    })
    drainQueue()
  })
}

function responseError(status: number, body: unknown) {
  const message = typeof body === 'object' && body && 'error' in body
    ? String((body as { error?: unknown }).error)
    : ''
  if (status === 401 || status === 403) {
    return new FinnhubError(message || 'Finnhub rejected this API key or endpoint entitlement.', status === 403 ? 'ENTITLEMENT' : 'UNAUTHORIZED', status)
  }
  if (status === 429) return new FinnhubError('Finnhub rate limit reached. Wait before refreshing.', 'RATE_LIMITED', status)
  return new FinnhubError(message || `Finnhub request failed with HTTP ${status}.`, 'NETWORK', status)
}

async function request<T>(
  key: string,
  path: string,
  params: Record<string, string | number>,
  apiKey: string,
  ttlMs: number,
  signal?: AbortSignal,
  allowStale = true,
): Promise<{ value: T; cached: boolean; stale: boolean }> {
  if (!apiKey.trim()) throw new FinnhubError('Enter a Finnhub API key to load ETF data.', 'MISSING_KEY')
  const cached = await readCache<T>(key)
  if (cached && cacheIsFresh(cached)) return { value: cached.value, cached: true, stale: false }
  const existing = inFlight.get(key) as Promise<{ value: T; cached: boolean; stale: boolean }> | undefined
  if (existing) return existing

  const operation = schedule(async () => {
    const url = new URL(`${BASE_URL}${path}`)
    Object.entries(params).forEach(([name, value]) => url.searchParams.set(name, String(value)))
    url.searchParams.set('token', apiKey)
    try {
      const response = await fetch(url, { signal, headers: { Accept: 'application/json' } })
      const body = await response.json().catch(() => null)
      if (!response.ok) throw responseError(response.status, body)
      if (typeof body === 'object' && body && 'error' in body) {
        const message = String((body as { error?: unknown }).error ?? 'Finnhub rejected this request.')
        if (message.toLowerCase().includes('access') || message.toLowerCase().includes('premium')) {
          throw new FinnhubError(message, 'ENTITLEMENT', response.status)
        }
        if (message.toLowerCase().includes('limit')) {
          throw new FinnhubError(message, 'RATE_LIMITED', response.status)
        }
        throw new FinnhubError(message, 'INVALID_RESPONSE', response.status)
      }
      await writeCache(key, body as T, ttlMs)
      return { value: body as T, cached: false, stale: false }
    } catch (error) {
      if (
        cached
        && allowStale
        && error instanceof FinnhubError
        && (error.code === 'NETWORK' || error.code === 'RATE_LIMITED')
      ) {
        return { value: cached.value, cached: true, stale: true }
      }
      if (error instanceof FinnhubError || error instanceof DOMException) throw error
      if (cached && allowStale) return { value: cached.value, cached: true, stale: true }
      throw new FinnhubError(error instanceof Error ? error.message : 'Unable to reach Finnhub.', 'NETWORK')
    }
  }, signal)
  inFlight.set(key, operation)
  try {
    return await operation
  } finally {
    inFlight.delete(key)
  }
}

type RawQuote = { c?: number; d?: number; dp?: number; h?: number; l?: number; o?: number; pc?: number; t?: number }
type RawCandles = { s?: string; c?: number[]; h?: number[]; l?: number[]; o?: number[]; t?: number[]; v?: number[] }

export async function getQuote(symbol: string, apiKey: string, signal?: AbortSignal) {
  const response = await request<RawQuote>(`quote:${symbol}`, '/quote', { symbol }, apiKey, 60_000, signal)
  const raw = response.value
  if (![raw.c, raw.h, raw.l, raw.o, raw.pc, raw.t].every(value => typeof value === 'number')) {
    throw new FinnhubError(`Finnhub returned an incomplete quote for ${symbol}.`, 'INVALID_RESPONSE')
  }
  const quote: Quote = {
    current: raw.c!,
    change: raw.d ?? raw.c! - raw.pc!,
    changePercent: raw.dp ?? (raw.pc ? (raw.c! / raw.pc - 1) * 100 : 0),
    high: raw.h!,
    low: raw.l!,
    open: raw.o!,
    previousClose: raw.pc!,
    timestamp: raw.t!,
  }
  return { ...response, value: quote }
}

function normalizeCandles(raw: RawCandles, symbol: string): Candle[] {
  if (raw.s === 'no_data') return []
  const lengths = [raw.c, raw.h, raw.l, raw.o, raw.t, raw.v].map(values => values?.length ?? 0)
  if (!lengths[0] || !lengths.every(length => length === lengths[0])) {
    throw new FinnhubError(`Finnhub returned incomplete candle arrays for ${symbol}.`, 'INVALID_RESPONSE')
  }
  const unique = new Map<number, Candle>()
  raw.t!.forEach((time, index) => {
    const candle: Candle = {
      time,
      open: raw.o![index]!,
      high: raw.h![index]!,
      low: raw.l![index]!,
      close: raw.c![index]!,
      volume: raw.v![index]!,
    }
    if ([candle.open, candle.high, candle.low, candle.close].every(value => Number.isFinite(value) && value > 0)) {
      unique.set(time, candle)
    }
  })
  return [...unique.values()].sort((left, right) => left.time - right.time)
}

export async function getCandles(symbol: string, range: ChartRange, apiKey: string, signal?: AbortSignal) {
  const config = chartRangeRequest(range)
  const maxChunkSeconds = 28 * 86_400
  const chunks: Array<{ from: number; to: number }> = []
  if (isIntradayResolution(config.resolution) && config.to - config.from > maxChunkSeconds) {
    for (let from = config.from; from < config.to; from += maxChunkSeconds) {
      chunks.push({ from, to: Math.min(from + maxChunkSeconds, config.to) })
    }
  } else {
    chunks.push({ from: config.from, to: config.to })
  }

  const responses = await Promise.all(chunks.map(chunk => request<RawCandles>(
    `candles:${symbol}:${config.resolution}:${chunk.from}:${chunk.to}`,
    '/stock/candle',
    { symbol, resolution: config.resolution, from: chunk.from, to: chunk.to },
    apiKey,
    config.cacheTtlMs,
    signal,
  )))
  const deduplicated = new Map<number, Candle>()
  responses.flatMap(response => normalizeCandles(response.value, symbol)).forEach(candle => deduplicated.set(candle.time, candle))
  return {
    value: [...deduplicated.values()].sort((left, right) => left.time - right.time),
    cached: responses.every(response => response.cached),
    stale: responses.some(response => response.stale),
    resolution: config.resolution,
    resolutionLabel: config.label,
  }
}

export async function getProfile(symbol: string, apiKey: string, signal?: AbortSignal) {
  return request<FinnhubProfile>(`profile:${symbol}`, '/stock/profile2', { symbol }, apiKey, 7 * 86_400_000, signal)
}

type RawEtfProfile = EtfProfile & { holding?: EtfHolding[]; sectorExposure?: Array<{ sector?: string; exposure?: number }> }

export async function getEtfProfile(symbol: string, apiKey: string, signal?: AbortSignal) {
  const response = await request<RawEtfProfile>(`etf-profile:${symbol}`, '/etf/profile', { symbol }, apiKey, 86_400_000, signal)
  const raw = response.value
  return {
    ...response,
    value: {
      ...raw,
      holdings: raw.holdings ?? raw.holding ?? [],
      sectorExposure: raw.sectorExposure?.reduce<Record<string, number>>((output, item) => {
        if (item.sector && typeof item.exposure === 'number') output[item.sector] = item.exposure
        return output
      }, {}) ?? {},
    } satisfies EtfProfile,
  }
}
