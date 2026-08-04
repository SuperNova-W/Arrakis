export type EtfDefinition = {
  symbol: string
  name: string
  category: 'sector' | 'context'
}

export const ETF_UNIVERSE: EtfDefinition[] = [
  { symbol: 'XLC', name: 'Communication Services', category: 'sector' },
  { symbol: 'XLY', name: 'Consumer Discretionary', category: 'sector' },
  { symbol: 'XLP', name: 'Consumer Staples', category: 'sector' },
  { symbol: 'XLE', name: 'Energy', category: 'sector' },
  { symbol: 'XLF', name: 'Financials', category: 'sector' },
  { symbol: 'XLV', name: 'Health Care', category: 'sector' },
  { symbol: 'XLI', name: 'Industrials', category: 'sector' },
  { symbol: 'XLB', name: 'Materials', category: 'sector' },
  { symbol: 'XLRE', name: 'Real Estate', category: 'sector' },
  { symbol: 'XLK', name: 'Technology', category: 'sector' },
  { symbol: 'XLU', name: 'Utilities', category: 'sector' },
  { symbol: 'SPY', name: 'S&P 500', category: 'context' },
  { symbol: 'QQQ', name: 'Nasdaq-100', category: 'context' },
  { symbol: 'IWM', name: 'Russell 2000', category: 'context' },
  { symbol: 'TLT', name: 'Long-Term Treasuries', category: 'context' },
  { symbol: 'HYG', name: 'High-Yield Corporate Bonds', category: 'context' },
  { symbol: 'GLD', name: 'Gold', category: 'context' },
  { symbol: 'USO', name: 'Crude Oil', category: 'context' },
]

export function findEtf(symbol: string) {
  return ETF_UNIVERSE.find(etf => etf.symbol === symbol)
}
