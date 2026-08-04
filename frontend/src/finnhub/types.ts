export type Candle = {
  time: number
  open: number
  high: number
  low: number
  close: number
  volume: number
}

export type Quote = {
  current: number
  change: number
  changePercent: number
  high: number
  low: number
  open: number
  previousClose: number
  timestamp: number
}

export type FinnhubProfile = {
  country?: string
  currency?: string
  exchange?: string
  finnhubIndustry?: string
  logo?: string
  marketCapitalization?: number
  name?: string
  ticker?: string
  weburl?: string
}

export type EtfHolding = {
  symbol?: string
  name?: string
  share?: number
  percent?: number
  value?: number
}

export type EtfProfile = {
  name?: string
  ticker?: string
  isin?: string
  cusip?: string
  assetClass?: string
  expenseRatio?: number
  nav?: number
  aum?: number
  inceptionDate?: string
  holdings?: EtfHolding[]
  sectorExposure?: Record<string, number>
}

export type ChartRange = '1D' | '5D' | '1M' | '3M' | '6M' | 'YTD' | '1Y' | '5Y' | 'MAX'

export type ChartStyle = 'area' | 'candles'

export type IndicatorKey = 'sma20' | 'sma50' | 'sma200' | 'ema20' | 'bollinger' | 'rsi' | 'macd'

export type NumericPoint = { time: number; value: number }
export type HistogramPoint = { time: number; value: number; color?: string }

export type IndicatorBundle = {
  sma20: NumericPoint[]
  sma50: NumericPoint[]
  sma200: NumericPoint[]
  ema20: NumericPoint[]
  bollingerUpper: NumericPoint[]
  bollingerLower: NumericPoint[]
  rsi: NumericPoint[]
  macd: NumericPoint[]
  macdSignal: NumericPoint[]
  macdHistogram: HistogramPoint[]
}
