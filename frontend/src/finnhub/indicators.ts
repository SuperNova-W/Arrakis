import type { Candle, HistogramPoint, IndicatorBundle, NumericPoint } from './types'

function simpleMovingAverage(candles: Candle[], period: number): NumericPoint[] {
  const result: NumericPoint[] = []
  let sum = 0
  candles.forEach((candle, index) => {
    sum += candle.close
    if (index >= period) sum -= candles[index - period]!.close
    if (index >= period - 1) result.push({ time: candle.time, value: sum / period })
  })
  return result
}

function exponentialMovingAverage(candles: Candle[], period: number): NumericPoint[] {
  if (candles.length < period) return []
  const multiplier = 2 / (period + 1)
  let value = candles.slice(0, period).reduce((sum, candle) => sum + candle.close, 0) / period
  const result: NumericPoint[] = [{ time: candles[period - 1]!.time, value }]
  for (let index = period; index < candles.length; index += 1) {
    const candle = candles[index]!
    value = (candle.close - value) * multiplier + value
    result.push({ time: candle.time, value })
  }
  return result
}

function bollinger(candles: Candle[], period = 20, deviations = 2) {
  const upper: NumericPoint[] = []
  const lower: NumericPoint[] = []
  for (let index = period - 1; index < candles.length; index += 1) {
    const window = candles.slice(index - period + 1, index + 1)
    const average = window.reduce((sum, candle) => sum + candle.close, 0) / period
    const variance = window.reduce((sum, candle) => sum + (candle.close - average) ** 2, 0) / period
    const width = Math.sqrt(variance) * deviations
    upper.push({ time: candles[index]!.time, value: average + width })
    lower.push({ time: candles[index]!.time, value: average - width })
  }
  return { upper, lower }
}

function relativeStrengthIndex(candles: Candle[], period = 14): NumericPoint[] {
  if (candles.length <= period) return []
  let gains = 0
  let losses = 0
  for (let index = 1; index <= period; index += 1) {
    const difference = candles[index]!.close - candles[index - 1]!.close
    if (difference >= 0) gains += difference
    else losses -= difference
  }
  let averageGain = gains / period
  let averageLoss = losses / period
  const output: NumericPoint[] = []
  const push = (index: number) => {
    const rsi = averageLoss === 0 ? 100 : 100 - 100 / (1 + averageGain / averageLoss)
    output.push({ time: candles[index]!.time, value: rsi })
  }
  push(period)
  for (let index = period + 1; index < candles.length; index += 1) {
    const difference = candles[index]!.close - candles[index - 1]!.close
    averageGain = (averageGain * (period - 1) + Math.max(difference, 0)) / period
    averageLoss = (averageLoss * (period - 1) + Math.max(-difference, 0)) / period
    push(index)
  }
  return output
}

function emaValues(values: NumericPoint[], period: number): NumericPoint[] {
  if (values.length < period) return []
  const multiplier = 2 / (period + 1)
  let current = values.slice(0, period).reduce((sum, point) => sum + point.value, 0) / period
  const output: NumericPoint[] = [{ time: values[period - 1]!.time, value: current }]
  for (let index = period; index < values.length; index += 1) {
    const point = values[index]!
    current = (point.value - current) * multiplier + current
    output.push({ time: point.time, value: current })
  }
  return output
}

function macd(candles: Candle[]) {
  const fast = exponentialMovingAverage(candles, 12)
  const slow = exponentialMovingAverage(candles, 26)
  const fastMap = new Map(fast.map(point => [point.time, point.value]))
  const line: NumericPoint[] = slow.flatMap(point => {
    const fastValue = fastMap.get(point.time)
    return fastValue == null ? [] : [{ time: point.time, value: fastValue - point.value }]
  })
  const signal = emaValues(line, 9)
  const signalMap = new Map(signal.map(point => [point.time, point.value]))
  const histogram: HistogramPoint[] = line.flatMap(point => {
    const signalValue = signalMap.get(point.time)
    if (signalValue == null) return []
    const value = point.value - signalValue
    return [{ time: point.time, value, color: value >= 0 ? '#57a88f' : '#d16d6d' }]
  })
  return { line, signal, histogram }
}

export function calculateIndicators(candles: Candle[]): IndicatorBundle {
  const bands = bollinger(candles)
  const macdValues = macd(candles)
  return {
    sma20: simpleMovingAverage(candles, 20),
    sma50: simpleMovingAverage(candles, 50),
    sma200: simpleMovingAverage(candles, 200),
    ema20: exponentialMovingAverage(candles, 20),
    bollingerUpper: bands.upper,
    bollingerLower: bands.lower,
    rsi: relativeStrengthIndex(candles),
    macd: macdValues.line,
    macdSignal: macdValues.signal,
    macdHistogram: macdValues.histogram,
  }
}

export type EtfStatistics = {
  returnPercent: number
  annualizedVolatility: number
  maximumDrawdown: number
  periodHigh: number
  periodLow: number
  averageVolume: number
  positiveSessions: number
}

export function calculateStatistics(candles: Candle[], periodsPerYear: number): EtfStatistics | null {
  if (candles.length < 2) return null
  const returns = candles.slice(1).map((candle, index) => Math.log(candle.close / candles[index]!.close))
  const mean = returns.reduce((sum, value) => sum + value, 0) / returns.length
  const variance = returns.reduce((sum, value) => sum + (value - mean) ** 2, 0) / Math.max(1, returns.length - 1)
  let peak = candles[0]!.close
  let maximumDrawdown = 0
  candles.forEach(candle => {
    peak = Math.max(peak, candle.close)
    maximumDrawdown = Math.min(maximumDrawdown, candle.close / peak - 1)
  })
  return {
    returnPercent: (candles.at(-1)!.close / candles[0]!.open - 1) * 100,
    annualizedVolatility: Math.sqrt(variance) * Math.sqrt(periodsPerYear) * 100,
    maximumDrawdown: maximumDrawdown * 100,
    periodHigh: Math.max(...candles.map(candle => candle.high)),
    periodLow: Math.min(...candles.map(candle => candle.low)),
    averageVolume: candles.reduce((sum, candle) => sum + candle.volume, 0) / candles.length,
    positiveSessions: returns.filter(value => value > 0).length / returns.length * 100,
  }
}
