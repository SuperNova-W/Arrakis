export type SignalDirection = 'LONG' | 'FLAT' | 'SHORT'
export type ServiceState = 'HEALTHY' | 'DEGRADED' | 'OFFLINE'

export interface EquityPoint {
  date: string
  strategy: number
  benchmark: number
  drawdown: number
}

export interface SignalEvent {
  id: string
  time: string
  symbol: string
  price: number
  direction: SignalDirection
  confidence: number
  model: string
  regime: string
}

export interface FeatureContribution {
  name: string
  value: number
  contribution: number
  direction: 'positive' | 'negative'
}

export interface ServiceMetric {
  name: string
  description: string
  state: ServiceState
  throughput: string
  latency: string
  consumerLag: number
}

const compactDate = (date: Date) =>
  date.toLocaleDateString('en-US', { month: 'short', day: 'numeric' })

export const equityCurve: EquityPoint[] = Array.from({ length: 126 }, (_, index) => {
  const date = new Date(2026, 0, 2 + index)
  const marketNoise = Math.sin(index * 0.27) * 0.008 + Math.cos(index * 0.11) * 0.004
  const strategy = 100 * Math.exp(index * 0.00165 + marketNoise)
  const benchmark = 100 * Math.exp(index * 0.00092 + Math.sin(index * 0.19) * 0.012)
  const rollingPeak = 100 * Math.exp(index * 0.0017 + 0.009)

  return {
    date: compactDate(date),
    strategy: Number(strategy.toFixed(2)),
    benchmark: Number(benchmark.toFixed(2)),
    drawdown: Number(((strategy / rollingPeak - 1) * 100).toFixed(2)),
  }
})

export const recentSignals: SignalEvent[] = [
  {
    id: 'evt-6a82',
    time: '15:58:42.184',
    symbol: 'IWM',
    price: 227.8,
    direction: 'LONG',
    confidence: 68.4,
    model: 'xgb-v0.8.3',
    regime: 'Risk-on',
  },
  {
    id: 'evt-6a81',
    time: '15:58:41.902',
    symbol: 'SPY',
    price: 629.73,
    direction: 'FLAT',
    confidence: 52.1,
    model: 'xgb-v0.8.3',
    regime: 'Risk-on',
  },
  {
    id: 'evt-6a80',
    time: '15:58:41.618',
    symbol: 'QQQ',
    price: 556.15,
    direction: 'LONG',
    confidence: 64.7,
    model: 'xgb-v0.8.3',
    regime: 'High momentum',
  },
  {
    id: 'evt-6a79',
    time: '15:58:40.991',
    symbol: 'TLT',
    price: 88.42,
    direction: 'SHORT',
    confidence: 61.3,
    model: 'xgb-v0.8.3',
    regime: 'Rates rising',
  },
  {
    id: 'evt-6a78',
    time: '15:58:40.442',
    symbol: 'VXX',
    price: 47.06,
    direction: 'FLAT',
    confidence: 55.9,
    model: 'logreg-v0.4.1',
    regime: 'Low volatility',
  },
  {
    id: 'evt-6a77',
    time: '15:58:39.873',
    symbol: 'DIA',
    price: 449.51,
    direction: 'LONG',
    confidence: 59.8,
    model: 'xgb-v0.8.3',
    regime: 'Risk-on',
  },
]

export const featureContributions: FeatureContribution[] = [
  { name: 'IWM / SPY relative strength', value: 1.42, contribution: 18.7, direction: 'positive' },
  { name: '21-day momentum', value: 4.81, contribution: 16.2, direction: 'positive' },
  { name: 'Volatility regime', value: 0.74, contribution: 11.8, direction: 'positive' },
  { name: 'Volume z-score', value: -0.31, contribution: 8.4, direction: 'negative' },
  { name: 'Overnight gap', value: -0.18, contribution: 6.9, direction: 'negative' },
  { name: '200-day trend distance', value: 7.26, contribution: 5.7, direction: 'positive' },
]

export const foldPerformance = Array.from({ length: 24 }, (_, index) => ({
  fold: `F${index + 20}`,
  auc: Number((0.5 + Math.sin(index * 1.37) * 0.054 + (index % 5) * 0.005).toFixed(3)),
  sharpe: Number((0.42 + Math.sin(index * 0.93) * 0.74).toFixed(2)),
}))

export const services: ServiceMetric[] = [
  {
    name: 'market-ingest',
    description: 'Raw OHLCV producer',
    state: 'HEALTHY',
    throughput: '8.4k msg/s',
    latency: '4.2 ms',
    consumerLag: 0,
  },
  {
    name: 'event-validator',
    description: 'Schema and quality gate',
    state: 'HEALTHY',
    throughput: '8.3k msg/s',
    latency: '6.8 ms',
    consumerLag: 12,
  },
  {
    name: 'feature-engine',
    description: 'Stateful rolling features',
    state: 'HEALTHY',
    throughput: '8.2k msg/s',
    latency: '11.6 ms',
    consumerLag: 31,
  },
  {
    name: 'model-inference',
    description: 'Versioned XGBoost scoring',
    state: 'HEALTHY',
    throughput: '8.2k msg/s',
    latency: '3.7 ms',
    consumerLag: 4,
  },
  {
    name: 'risk-gateway',
    description: 'Exposure and position limits',
    state: 'DEGRADED',
    throughput: '7.9k msg/s',
    latency: '28.4 ms',
    consumerLag: 146,
  },
  {
    name: 'signal-writer',
    description: 'PostgreSQL persistence',
    state: 'HEALTHY',
    throughput: '7.9k msg/s',
    latency: '9.1 ms',
    consumerLag: 8,
  },
]

export const topicMetrics = [
  { topic: 'market.raw.v1', partitions: 12, rate: '8,412/s', retention: '7d', size: '182 GB' },
  { topic: 'market.validated.v1', partitions: 12, rate: '8,301/s', retention: '7d', size: '176 GB' },
  { topic: 'features.realtime.v1', partitions: 12, rate: '8,247/s', retention: '3d', size: '94 GB' },
  { topic: 'signals.model.v1', partitions: 6, rate: '1,128/s', retention: '30d', size: '21 GB' },
  { topic: 'signals.dlq.v1', partitions: 3, rate: '0.02/s', retention: '30d', size: '14 MB' },
]
