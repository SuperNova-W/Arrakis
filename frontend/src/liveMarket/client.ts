import type {
  ConnectionStatus,
  HeartbeatMessage,
  LiveBar,
  LiveMarketMessage,
  LiveTrade,
  LiveUpdate,
  StreamStatusMessage,
} from './types'

type MessageListener = (message: LiveMarketMessage | null, status: ConnectionStatus) => void

const MAX_RECONNECTING_ATTEMPTS = 5
const MAX_RECONNECT_DELAY_MS = 30_000
const OFFLINE_RETRY_DELAY_MS = 5 * 60_000

function websocketEndpoint() {
  const configured = import.meta.env.VITE_MARKET_API_WS_URL?.trim()
  if (configured) return configured
  if (typeof window === 'undefined') return ''
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
  return `${protocol}//${window.location.hostname}:8080/ws/v1/market`
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null
}

function isString(value: unknown): value is string {
  return typeof value === 'string'
}

function isFiniteNumber(value: unknown): value is number {
  return typeof value === 'number' && Number.isFinite(value)
}

function isLiveTrade(value: unknown): value is LiveTrade {
  if (!isRecord(value)) return false
  return isString(value.event_id)
    && isString(value.symbol)
    && isFiniteNumber(value.price)
    && isFiniteNumber(value.volume)
    && isString(value.timestamp)
    && isString(value.source)
}

function isLiveBar(value: unknown): value is LiveBar {
  if (!isRecord(value)) return false
  return isString(value.bar_id)
    && isString(value.symbol)
    && isString(value.interval)
    && isString(value.bar_start)
    && isString(value.bar_end)
    && isFiniteNumber(value.open)
    && isFiniteNumber(value.high)
    && isFiniteNumber(value.low)
    && isFiniteNumber(value.close)
    && isFiniteNumber(value.volume)
    && isFiniteNumber(value.trade_count)
    && isString(value.first_trade_timestamp)
    && isString(value.last_trade_timestamp)
}

function parseMessage(payload: string): LiveMarketMessage | null {
  let value: unknown
  try {
    value = JSON.parse(payload) as unknown
  } catch {
    return null
  }
  if (!isRecord(value) || !isString(value.type)) return null

  if (
    value.type === 'market_update'
    && isFiniteNumber(value.sequence)
    && isLiveTrade(value.trade)
    && isLiveBar(value.one_minute)
    && isLiveBar(value.five_minute)
  ) {
    return value as unknown as LiveUpdate
  }
  if (value.type === 'stream_status' && isString(value.status)) {
    return value as unknown as StreamStatusMessage
  }
  if (value.type === 'heartbeat' && isFiniteNumber(value.sequence)) {
    return value as unknown as HeartbeatMessage
  }
  return null
}

class LiveMarketClient {
  private socket: WebSocket | null = null
  private reconnectTimer: number | null = null
  private reconnectAttempts = 0
  private status: ConnectionStatus = 'offline'
  private readonly listeners = new Set<MessageListener>()

  getStatus() {
    return this.status
  }

  subscribe(listener: MessageListener) {
    this.listeners.add(listener)
    if (this.listeners.size === 1) this.open()
    listener(null, this.status)
    return () => {
      this.listeners.delete(listener)
      if (!this.listeners.size) this.stop()
    }
  }

  private notify(message: LiveMarketMessage | null) {
    this.listeners.forEach(listener => listener(message, this.status))
  }

  private setStatus(status: ConnectionStatus) {
    if (this.status === status) return
    this.status = status
    this.notify(null)
  }

  private open() {
    const endpoint = websocketEndpoint()
    if (!endpoint || typeof WebSocket === 'undefined') {
      this.setStatus('offline')
      return
    }
    this.clearReconnectTimer()
    this.setStatus(this.reconnectAttempts ? 'reconnecting' : 'connecting')
    let socket: WebSocket
    try {
      socket = new WebSocket(endpoint)
    } catch {
      this.scheduleReconnect()
      return
    }
    this.socket = socket
    socket.onopen = () => {
      if (this.socket !== socket) return
      this.reconnectAttempts = 0
      this.setStatus('live')
    }
    socket.onmessage = event => {
      if (this.socket !== socket || typeof event.data !== 'string') return
      const message = parseMessage(event.data)
      if (message) this.notify(message)
    }
    socket.onerror = () => {
      if (this.socket === socket) this.setStatus('reconnecting')
    }
    socket.onclose = () => {
      if (this.socket !== socket) return
      this.socket = null
      this.scheduleReconnect()
    }
  }

  private scheduleReconnect() {
    if (!this.listeners.size) {
      this.setStatus('offline')
      return
    }
    // Cap attempts once exhausted rather than letting the counter grow unbounded for the
    // lifetime of a long-lived tab with no reachable backend.
    this.reconnectAttempts = Math.min(this.reconnectAttempts + 1, MAX_RECONNECTING_ATTEMPTS + 1)
    const exhausted = this.reconnectAttempts > MAX_RECONNECTING_ATTEMPTS
    this.setStatus(exhausted ? 'offline' : 'reconnecting')
    this.clearReconnectTimer()
    // After giving up on fast retries, fall back to a slow steady-state cadence so the UI can
    // still self-heal if the backend comes back, without hammering an unreachable host every 30s.
    const delay = exhausted
      ? OFFLINE_RETRY_DELAY_MS
      : Math.min(MAX_RECONNECT_DELAY_MS, 1_000 * 2 ** Math.min(this.reconnectAttempts - 1, 5))
    this.reconnectTimer = window.setTimeout(() => {
      this.reconnectTimer = null
      this.open()
    }, delay)
  }

  private clearReconnectTimer() {
    if (this.reconnectTimer !== null) {
      window.clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }
  }

  private stop() {
    this.clearReconnectTimer()
    this.reconnectAttempts = 0
    const socket = this.socket
    this.socket = null
    if (socket) {
      socket.onopen = null
      socket.onmessage = null
      socket.onerror = null
      socket.onclose = null
      socket.close()
    }
    this.setStatus('offline')
  }
}

export const liveMarketClient = new LiveMarketClient()
