import { cacheIsFresh, readCache, writeCache } from '../finnhub/cache'
import type { Candle, ChartRange } from '../finnhub/types'
import { twelveDataRangeRequest } from './ranges'

const BASE_URL = 'https://api.twelvedata.com'

export class TwelveDataError extends Error {
  constructor(
    message: string,
    readonly code: 'MISSING_KEY' | 'UNAUTHORIZED' | 'RATE_LIMITED' | 'NO_DATA' | 'INVALID_RESPONSE' | 'NETWORK',
    readonly status = 0,
  ) {
    super(message)
    this.name = 'TwelveDataError'
  }
}

type RawValue = { datetime: string; open: string; high: string; low: string; close: string; volume: string }
type RawResponse =
  | { status: 'ok'; meta: { exchange_timezone: string }; values: RawValue[] }
  | { status: 'error'; code: number; message: string }

// Twelve Data intraday timestamps are wall-clock time in the exchange's local timezone (see
// meta.exchange_timezone), not UTC. Converting them correctly requires knowing that zone's UTC
// offset at the given instant (it changes across DST transitions).
function exchangeTimeToUnixSeconds(datetime: string, timeZone: string): number {
  const [datePart, timePart] = datetime.split(' ')
  const [year, month, day] = datePart!.split('-').map(Number)
  const [hour, minute, second] = (timePart ?? '00:00:00').split(':').map(Number)
  const utcGuess = Date.UTC(year!, month! - 1, day!, hour ?? 0, minute ?? 0, second ?? 0)

  const parts = new Intl.DateTimeFormat('en-US', {
    timeZone,
    timeZoneName: 'shortOffset',
  }).formatToParts(new Date(utcGuess))
  const offsetLabel = parts.find(part => part.type === 'timeZoneName')?.value ?? 'GMT+0'
  const match = /GMT([+-])(\d{1,2})(?::?(\d{2}))?/.exec(offsetLabel)
  const sign = match?.[1] === '-' ? -1 : 1
  const offsetMs = match ? sign * ((Number(match[2]) * 60 + Number(match[3] ?? 0)) * 60_000) : 0

  return Math.floor((utcGuess - offsetMs) / 1000)
}

function normalizeValues(values: RawValue[], timeZone: string): Candle[] {
  const candles = values.map((value): Candle => ({
    time: exchangeTimeToUnixSeconds(value.datetime, timeZone),
    open: Number.parseFloat(value.open),
    high: Number.parseFloat(value.high),
    low: Number.parseFloat(value.low),
    close: Number.parseFloat(value.close),
    volume: Number.parseFloat(value.volume),
  })).filter(candle => [candle.open, candle.high, candle.low, candle.close].every(value => Number.isFinite(value) && value > 0))
  return candles.sort((left, right) => left.time - right.time)
}

export async function getCandles(symbol: string, range: ChartRange, apiKey: string, signal?: AbortSignal) {
  if (!apiKey.trim()) throw new TwelveDataError('Missing Twelve Data API key.', 'MISSING_KEY')
  const config = twelveDataRangeRequest(range)
  const cacheKey = `td:candles:${symbol}:${config.interval}:${config.outputSize}`

  const cached = await readCache<RawResponse>(cacheKey)
  if (cached && cacheIsFresh(cached) && cached.value.status === 'ok') {
    return { value: normalizeValues(cached.value.values, cached.value.meta.exchange_timezone), cached: true, stale: false, resolutionLabel: config.label }
  }

  const url = new URL(`${BASE_URL}/time_series`)
  url.searchParams.set('symbol', symbol)
  url.searchParams.set('interval', config.interval)
  url.searchParams.set('outputsize', String(config.outputSize))
  url.searchParams.set('apikey', apiKey)

  try {
    const response = await fetch(url, { signal, headers: { Accept: 'application/json' } })
    const body = (await response.json().catch(() => null)) as RawResponse | null
    if (!body) throw new TwelveDataError('Twelve Data returned an unreadable response.', 'INVALID_RESPONSE', response.status)
    if (body.status === 'error') {
      if (body.code === 401 || body.code === 403) throw new TwelveDataError(body.message, 'UNAUTHORIZED', body.code)
      if (body.code === 429) throw new TwelveDataError(body.message, 'RATE_LIMITED', body.code)
      if (body.code === 404) throw new TwelveDataError(body.message, 'NO_DATA', body.code)
      throw new TwelveDataError(body.message, 'INVALID_RESPONSE', body.code)
    }
    if (!response.ok) throw new TwelveDataError(`Twelve Data request failed with HTTP ${response.status}.`, 'NETWORK', response.status)
    await writeCache(cacheKey, body, config.cacheTtlMs)
    return { value: normalizeValues(body.values, body.meta.exchange_timezone), cached: false, stale: false, resolutionLabel: config.label }
  } catch (error) {
    if (error instanceof TwelveDataError) {
      if (cached && (error.code === 'NETWORK' || error.code === 'RATE_LIMITED') && cached.value.status === 'ok') {
        return { value: normalizeValues(cached.value.values, cached.value.meta.exchange_timezone), cached: true, stale: true, resolutionLabel: config.label }
      }
      throw error
    }
    if (error instanceof DOMException) throw error
    throw new TwelveDataError(error instanceof Error ? error.message : 'Unable to reach Twelve Data.', 'NETWORK')
  }
}
