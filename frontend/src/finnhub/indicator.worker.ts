import { calculateIndicators } from './indicators'
import type { Candle } from './types'

self.onmessage = (event: MessageEvent<Candle[]>) => {
  self.postMessage(calculateIndicators(event.data))
}
