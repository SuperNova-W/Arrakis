export type LiveTrade = {
  event_id: string
  symbol: string
  price: number
  volume: number
  timestamp: string
  source: string
}

export type LiveBar = {
  bar_id: string
  symbol: string
  interval: string
  bar_start: string
  bar_end: string
  open: number
  high: number
  low: number
  close: number
  volume: number
  trade_count: number
  first_trade_timestamp: string
  last_trade_timestamp: string
}

export type LiveUpdate = {
  type: 'market_update'
  sequence: number
  trade: LiveTrade
  one_minute: LiveBar
  five_minute: LiveBar
}

export type StreamStatusMessage = {
  type: 'stream_status'
  status: string
  source?: string
}

export type HeartbeatMessage = {
  type: 'heartbeat'
  sequence: number
  source?: string
}

export type LiveMarketMessage = LiveUpdate | StreamStatusMessage | HeartbeatMessage

export type ConnectionStatus = 'connecting' | 'live' | 'reconnecting' | 'offline'
