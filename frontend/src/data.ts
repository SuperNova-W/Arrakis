export type Signal = 'Bullish' | 'Neutral' | 'Bearish'
export type Sector = { symbol: string; name: string; price: number; change: number; signal: Signal; predicted: number; confidence: number; strength: number; model: string; updated: string; rationale: string; volume: string }
export const sectors: Sector[] = [
  { symbol:'XLK', name:'Technology', price:230.61, change:1.42, signal:'Bullish', predicted:0.68, confidence:78, strength:92, model:'xgb-tech-v3.4', updated:'10:31:42', rationale:'Momentum remains above the 20D trend with improving breadth and positive volume skew.', volume:'1.34×' },
  { symbol:'XLF', name:'Financials', price:49.22, change:0.87, signal:'Bullish', predicted:0.41, confidence:69, strength:81, model:'xgb-fin-v3.4', updated:'10:31:38', rationale:'Relative strength is expanding against SPY; volatility regime remains supportive.', volume:'1.11×' },
  { symbol:'XLI', name:'Industrials', price:139.48, change:0.52, signal:'Bullish', predicted:0.29, confidence:64, strength:67, model:'xgb-ind-v3.4', updated:'10:31:35', rationale:'Trend and breadth are constructive, though short-term acceleration has moderated.', volume:'0.96×' },
  { symbol:'XLE', name:'Energy', price:88.17, change:-0.36, signal:'Neutral', predicted:0.04, confidence:55, strength:48, model:'xgb-enr-v3.4', updated:'10:31:29', rationale:'Price is range-bound as commodity momentum offsets weakening sector breadth.', volume:'1.02×' },
  { symbol:'XLV', name:'Health Care', price:146.03, change:-0.18, signal:'Neutral', predicted:-0.03, confidence:52, strength:44, model:'xgb-hlth-v3.4', updated:'10:31:26', rationale:'Defensive flows are steady, but the model sees limited near-term directional edge.', volume:'0.89×' },
  { symbol:'XLY', name:'Consumer Discretionary', price:208.66, change:0.13, signal:'Neutral', predicted:0.02, confidence:51, strength:42, model:'xgb-cyc-v3.4', updated:'10:31:21', rationale:'Mixed breadth and neutral volatility inputs keep the recommendation in the middle band.', volume:'0.92×' },
  { symbol:'XLP', name:'Consumer Staples', price:81.36, change:-0.24, signal:'Bearish', predicted:-0.31, confidence:63, strength:28, model:'xgb-stap-v3.4', updated:'10:31:17', rationale:'Relative weakness persists as downside breadth and trend slope both deteriorate.', volume:'1.08×' },
  { symbol:'XLU', name:'Utilities', price:82.94, change:-0.61, signal:'Bearish', predicted:-0.44, confidence:71, strength:19, model:'xgb-util-v3.4', updated:'10:31:11', rationale:'Rate sensitivity and negative trend alignment are driving the strongest downside signal.', volume:'1.26×' },
  { symbol:'XLB', name:'Materials', price:91.07, change:-0.42, signal:'Bearish', predicted:-0.26, confidence:59, strength:24, model:'xgb-matl-v3.4', updated:'10:31:07', rationale:'Weakening commodity complex and sub-50 breadth are weighing on the forecast.', volume:'0.98×' },
  { symbol:'XLC', name:'Communication Services', price:95.18, change:0.31, signal:'Neutral', predicted:0.08, confidence:54, strength:55, model:'xgb-comm-v3.4', updated:'10:30:58', rationale:'Price holds above trend, but dispersion across constituents lowers conviction.', volume:'0.87×' },
  { symbol:'XLRE', name:'Real Estate', price:41.52, change:-0.12, signal:'Neutral', predicted:0.01, confidence:49, strength:38, model:'xgb-re-v3.4', updated:'10:30:51', rationale:'Cross-asset inputs are balanced; model output is close to its neutral threshold.', volume:'0.78×' },
]
export const contexts = [{s:'SPY',p:531.44,c:0.64},{s:'QQQ',p:457.92,c:1.08},{s:'IWM',p:201.32,c:-0.22},{s:'TLT',p:91.08,c:-0.47},{s:'HYG',p:79.64,c:0.12},{s:'GLD',p:217.51,c:0.36},{s:'USO',p:74.88,c:-0.83}]
const sessionClock = (index: number) => { const minutes = 9 * 60 + 30 + index * 5; return `${String(Math.floor(minutes / 60)).padStart(2, '0')}:${String(minutes % 60).padStart(2, '0')}` }
const intradayPath = (base: number, trend: number, wave: number, index: number) => Number((base + trend * index + Math.sin(index * .52) * wave + Math.sin(index * 1.41) * wave * .34 + Math.cos(index * .19) * wave * .58 + (index > 32 ? Math.sin(index * .23) * wave * .7 : 0)).toFixed(2))
export const chartData = Array.from({length: 79}, (_,i) => ({ time:sessionClock(i), XLK: intradayPath(227.18, .073, .76, i), SPY: intradayPath(528.26, .048, .62, i), XLU: intradayPath(84.26, -.018, .43, i) }))
export const predictions = sectors.flatMap((s, i) => [{...s, stamp:`10:${String(31-i).padStart(2,'0')}:42`},{...s, change:s.change-.22, predicted:s.predicted-.08, stamp:`10:${String(18-i).padStart(2,'0')}:12`}])

export type NewsSentiment = 'Positive' | 'Neutral' | 'Negative'
export type SectorNews = { id: string; time: string; source: string; headline: string; summary: string; sentiment: NewsSentiment; relevance: number; entities: string[] }
export type SectorResearch = { symbol: string; dayOpen: number; dayHigh: number; dayLow: number; prevClose: number; volume: string; vwap: number; forecastHorizon: string; probabilityUp: number; probabilityFlat: number; probabilityDown: number; newsCoverage: string; nlpStatus: string; news: SectorNews[] }

const newsBySector: Record<string, SectorNews[]> = {
  XLK: [
    { id:'xlk-1', time:'09:48 ET', source:'Market Desk', headline:'Semiconductor complex leads early sector rotation', summary:'Chip names are setting the pace as investors add exposure to large-cap technology while breadth improves.', sentiment:'Positive', relevance:94, entities:['Semiconductors','NASDAQ'] },
    { id:'xlk-2', time:'08:16 ET', source:'Macro Wire', headline:'Software earnings calendar keeps attention on AI capex', summary:'Upcoming results are expected to provide another read on enterprise AI spending and cloud demand.', sentiment:'Neutral', relevance:81, entities:['AI','Cloud'] },
    { id:'xlk-3', time:'07:42 ET', source:'Regulatory Brief', headline:'EU technology policy remains a monitoring item', summary:'Policy headlines are a modest source of uncertainty, though no new sector-level action was announced today.', sentiment:'Neutral', relevance:58, entities:['EU','Policy'] },
  ],
  XLF: [
    { id:'xlf-1', time:'10:02 ET', source:'Market Desk', headline:'Financials outperform as curve steepens modestly', summary:'Banks and diversified financials are trading with a positive relative-strength impulse against the broad market.', sentiment:'Positive', relevance:91, entities:['Banks','Treasuries'] },
    { id:'xlf-2', time:'08:54 ET', source:'Credit Monitor', headline:'Credit conditions remain stable into the open', summary:'Early credit indicators show limited stress across the monitored financial universe.', sentiment:'Neutral', relevance:73, entities:['Credit','Banks'] },
  ],
}

export const sectorResearch: Record<string, SectorResearch> = Object.fromEntries(sectors.map((sector) => {
  const base = sector.price - sector.change * 0.6
  return [sector.symbol, {
    symbol: sector.symbol, dayOpen: Number(base.toFixed(2)), dayHigh: Number((sector.price + 0.84).toFixed(2)), dayLow: Number((sector.price - 0.61).toFixed(2)), prevClose: Number((sector.price - sector.change / 100 * sector.price).toFixed(2)), volume: sector.volume, vwap: Number((sector.price - sector.change * 0.18).toFixed(2)), forecastHorizon:'Next 1 trading session', probabilityUp: sector.signal === 'Bullish' ? sector.confidence : Math.round(100 - sector.confidence * .36), probabilityFlat: sector.signal === 'Neutral' ? 48 : 24, probabilityDown: sector.signal === 'Bearish' ? sector.confidence : Math.round(100 - sector.confidence * .58), newsCoverage: newsBySector[sector.symbol]?.length ? '3 articles analyzed' : 'No source feed connected', nlpStatus: newsBySector[sector.symbol]?.length ? 'Sentiment model complete' : 'Awaiting scraper feed', news: newsBySector[sector.symbol] ?? [],
  }]
}))
