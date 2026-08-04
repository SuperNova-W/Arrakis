import type { ChartRange } from './types'

export type RangeRequest = {
  resolution: '1' | '5' | '30' | '60' | 'D' | 'W' | 'M'
  from: number
  to: number
  cacheTtlMs: number
  label: string
}

const DAY = 86_400

export function chartRangeRequest(range: ChartRange, now = new Date()): RangeRequest {
  const rawTo = Math.floor(now.getTime() / 1000)
  const minuteTo = Math.floor(rawTo / 60) * 60
  const dayTo = Math.floor(rawTo / DAY) * DAY
  const year = now.getUTCFullYear()
  const yearStart = Math.floor(Date.UTC(year, 0, 1) / 1000)
  switch (range) {
    case '1D': return { resolution: '1', from: minuteTo - DAY * 2, to: minuteTo, cacheTtlMs: 60_000, label: '1 minute' }
    case '5D': return { resolution: '5', from: minuteTo - DAY * 8, to: minuteTo, cacheTtlMs: 90_000, label: '5 minutes' }
    case '1M': return { resolution: '30', from: minuteTo - DAY * 32, to: minuteTo, cacheTtlMs: 5 * 60_000, label: '30 minutes' }
    case '3M': return { resolution: '60', from: minuteTo - DAY * 95, to: minuteTo, cacheTtlMs: 15 * 60_000, label: '60 minutes' }
    case '6M': return { resolution: 'D', from: dayTo - DAY * 190, to: dayTo, cacheTtlMs: 6 * 60 * 60_000, label: 'daily' }
    case 'YTD': return { resolution: 'D', from: yearStart, to: dayTo, cacheTtlMs: 6 * 60 * 60_000, label: 'daily' }
    case '1Y': return { resolution: 'D', from: dayTo - DAY * 370, to: dayTo, cacheTtlMs: 6 * 60 * 60_000, label: 'daily' }
    case '5Y': return { resolution: 'W', from: dayTo - DAY * 365 * 5 - DAY * 7, to: dayTo, cacheTtlMs: 24 * 60 * 60_000, label: 'weekly' }
    case 'MAX': return { resolution: 'M', from: Math.floor(Date.UTC(1990, 0, 1) / 1000), to: dayTo, cacheTtlMs: 24 * 60 * 60_000, label: 'monthly' }
  }
}

export function isIntradayResolution(resolution: RangeRequest['resolution']) {
  return resolution === '1' || resolution === '5' || resolution === '30' || resolution === '60'
}
