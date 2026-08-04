import type { ChartRange } from '../finnhub/types'

export type TwelveDataRangeRequest = {
  interval: '1min' | '5min' | '15min' | '30min' | '1h' | '1day' | '1week' | '1month'
  outputSize: number
  cacheTtlMs: number
  label: string
  intraday: boolean
}

export function twelveDataRangeRequest(range: ChartRange): TwelveDataRangeRequest {
  switch (range) {
    case '1D': return { interval: '1min', outputSize: 400, cacheTtlMs: 60_000, label: '1 minute', intraday: true }
    case '5D': return { interval: '5min', outputSize: 400, cacheTtlMs: 90_000, label: '5 minutes', intraday: true }
    case '1M': return { interval: '30min', outputSize: 300, cacheTtlMs: 5 * 60_000, label: '30 minutes', intraday: true }
    case '3M': return { interval: '1h', outputSize: 500, cacheTtlMs: 15 * 60_000, label: '60 minutes', intraday: true }
    case '6M': return { interval: '1day', outputSize: 130, cacheTtlMs: 6 * 60 * 60_000, label: 'daily', intraday: false }
    case 'YTD': return { interval: '1day', outputSize: 250, cacheTtlMs: 6 * 60 * 60_000, label: 'daily', intraday: false }
    case '1Y': return { interval: '1day', outputSize: 260, cacheTtlMs: 6 * 60 * 60_000, label: 'daily', intraday: false }
    case '5Y': return { interval: '1week', outputSize: 270, cacheTtlMs: 24 * 60 * 60_000, label: 'weekly', intraday: false }
    case 'MAX': return { interval: '1month', outputSize: 400, cacheTtlMs: 24 * 60 * 60_000, label: 'monthly', intraday: false }
  }
}
