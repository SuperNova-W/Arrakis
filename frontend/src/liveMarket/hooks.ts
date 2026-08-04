import { useEffect, useState } from 'react'
import { liveMarketClient } from './client'
import type { ConnectionStatus, LiveBar, LiveTrade } from './types'

type LiveMarketState = {
  symbol: string
  status: ConnectionStatus
  lastTrade: LiveTrade | null
  oneMinuteBar: LiveBar | null
  fiveMinuteBar: LiveBar | null
}

export type LiveMarketResult = Omit<LiveMarketState, 'symbol'>

function initialState(symbol: string): LiveMarketState {
  return {
    symbol,
    status: liveMarketClient.getStatus(),
    lastTrade: null,
    oneMinuteBar: null,
    fiveMinuteBar: null,
  }
}

export function useLiveMarket(symbol: string): LiveMarketResult {
  const [state, setState] = useState<LiveMarketState>(() => initialState(symbol))

  useEffect(() => {
    if (!symbol) return

    return liveMarketClient.subscribe((message, status) => {
      setState(current => {
        const next = current.symbol === symbol ? current : initialState(symbol)
        if (!message) return { ...next, status }
        if (message.type !== 'market_update' || message.trade.symbol !== symbol) {
          return { ...next, status }
        }
        return {
          symbol,
          status,
          lastTrade: message.trade,
          oneMinuteBar: message.one_minute,
          fiveMinuteBar: message.five_minute,
        }
      })
    })
  }, [symbol])

  const current = state.symbol === symbol ? state : initialState(symbol)
  return {
    status: current.status,
    lastTrade: current.lastTrade,
    oneMinuteBar: current.oneMinuteBar,
    fiveMinuteBar: current.fiveMinuteBar,
  }
}
