import { useMemo, useState } from 'react'
import {
  Activity,
  AlertTriangle,
  BarChart3,
  ChevronRight,
  Database,
  Download,
  BookOpen,
  ArrowUpRight,
  ArrowRight,
  ShieldCheck,
  RefreshCw,
  WifiOff,
} from 'lucide-react'
import FinnhubChart from './components/FinnhubChart'
import { ETF_UNIVERSE, findEtf, type EtfDefinition } from './etfUniverse'
import { FinnhubError } from './finnhub/client'
import { calculateStatistics } from './finnhub/indicators'
import { configuredFinnhubKey } from './finnhub/key'
import { useFinnhubProfile, useFinnhubQuote, useIndicators } from './finnhub/hooks'
import type { Candle, ChartRange, ChartStyle, IndicatorKey, Quote } from './finnhub/types'
import { TwelveDataError } from './twelveData/client'
import { useTwelveDataCandles } from './twelveData/hooks'
import { twelveDataRangeRequest } from './twelveData/ranges'
import { MlApiError, useMlRecommendation } from './mlApi'

const TWELVE_DATA_API_KEY = (import.meta.env.VITE_TWELVE_DATA_API_KEY ?? '').trim()

const RANGES: ChartRange[] = ['1D', '5D', '1M', '3M', '6M', 'YTD', '1Y', '5Y', 'MAX']
const INDICATORS: Array<{ key: IndicatorKey; label: string; kind: 'overlay' | 'pane' }> = [
  { key: 'sma20', label: 'SMA 20', kind: 'overlay' },
  { key: 'sma50', label: 'SMA 50', kind: 'overlay' },
  { key: 'sma200', label: 'SMA 200', kind: 'overlay' },
  { key: 'ema20', label: 'EMA 20', kind: 'overlay' },
  { key: 'bollinger', label: 'Bollinger', kind: 'overlay' },
  { key: 'rsi', label: 'RSI 14', kind: 'pane' },
  { key: 'macd', label: 'MACD', kind: 'pane' },
]

function Link({ to, children, className = '' }: { to: string; children: React.ReactNode; className?: string }) {
  return <a className={className} href={to}>{children}</a>
}

function NavLink({ to, children }: { to: string; children: React.ReactNode }) {
  const dashboardPath = window.location.pathname === '/' || window.location.pathname === '/recommendation'
  const active = to === '/' ? dashboardPath : window.location.pathname.startsWith(to)
  return <a className={active ? 'active' : ''} aria-current={active ? 'page' : undefined} href={to}>{children}</a>
}

function formatPrice(value?: number | null) {
  return value == null || !Number.isFinite(value) ? '—' : `$${value.toFixed(2)}`
}

function formatNumber(value?: number | null) {
  return value == null || !Number.isFinite(value) ? '—' : Math.round(value).toLocaleString()
}

function formatPercent(value?: number | null) {
  return value == null || !Number.isFinite(value) ? '—' : `${value >= 0 ? '+' : ''}${value.toFixed(2)}%`
}

function formatTimestamp(seconds?: number | null) {
  return seconds ? new Date(seconds * 1000).toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' }) : '—'
}

function marketStatus() {
  const parts = new Intl.DateTimeFormat('en-US', {
    timeZone: 'America/New_York',
    weekday: 'short',
    hour: '2-digit',
    minute: '2-digit',
    hour12: false,
  }).formatToParts(new Date())
  const weekday = parts.find(part => part.type === 'weekday')?.value ?? ''
  const hour = Number(parts.find(part => part.type === 'hour')?.value ?? 0)
  const minute = Number(parts.find(part => part.type === 'minute')?.value ?? 0)
  const clock = hour * 60 + minute
  const open = !['Sat', 'Sun'].includes(weekday) && clock >= 570 && clock < 960
  return open ? 'Market open' : 'Market closed'
}

function Shell({ children }: { children: React.ReactNode }) {
  return <div className="app-shell">
    <a className="skip-link" href="#main-content">Skip to content</a>
    <header className="site-header">
      <Link to="/" className="brand"><span className="brand-mark"><ArrowUpRight size={25} strokeWidth={3}/></span><span>ARRAKIS</span></Link>
      <nav className="top-nav" aria-label="Primary navigation">
        <NavLink to="/">OVERVIEW</NavLink>
        <a href="/#sectors">SECTORS</a>
        <a href="/#context">MARKET CONTEXT</a>
      </nav>
      <Link to="/etfs/XLK" className="header-cta">EXPLORE XLK <ArrowUpRight size={16}/></Link>
    </header>
    <main id="main-content" tabIndex={-1}>{children}</main>
    <footer className="site-footer"><Link to="/" className="brand">ARRAKIS<span className="footer-dot"/></Link><p>Independent research. Informed perspective.</p><div><ShieldCheck size={17}/><span>Research only. Not investment advice. No trades are executed.</span></div></footer>
  </div>
}

function Topbar({ eyebrow, title, children }: { eyebrow: string; title: string; children?: React.ReactNode }) {
  return <div className="topbar"><div><div className="eyebrow">{eyebrow}</div><h1>{title}</h1></div><div className="topbar-actions">{children}</div></div>
}

function FinnhubErrorState({ error, retry, compact = false, provider = 'Finnhub' }: { error: FinnhubError | TwelveDataError; retry?: () => void; compact?: boolean; provider?: string }) {
  const title = error.code === 'RATE_LIMITED' ? 'Prices are taking longer to update'
    : error.code === 'ENTITLEMENT' ? 'This information is not available'
    : error.code === 'NO_DATA' ? 'No prices for this period' : 'Prices are temporarily unavailable'
  const message = error.code === 'RATE_LIMITED' ? 'Please wait a minute, then try again.'
    : error.code === 'ENTITLEMENT' ? 'Our data provider does not currently supply this information for this fund.'
    : error.code === 'NO_DATA' ? 'Try choosing a different date range.' : 'We could not load the latest prices. Please try again shortly.'
  return <div role="status" className={`panel empty-state finnhub-error ${compact ? 'compact' : ''}`}>
    <WifiOff size={20}/><div><h2>{title}</h2><p>{message}</p><small>Source: {provider}</small>{retry && <button className="outline-btn" onClick={retry}><RefreshCw size={14}/> Try again</button>}</div>
  </div>
}

function MiniPriceChart({ symbol, quote }: { symbol: string; quote: Quote | null }) {
  if (!quote) return <div className="mini-chart mini-chart-empty">Day range unavailable</div>
  const position = quote.high === quote.low ? 50 : Math.max(0, Math.min(100, (quote.current - quote.low) / (quote.high - quote.low) * 100))
  return <div className="mini-chart" role="img" aria-label={symbol + ' current price ' + formatPrice(quote.current) + ' within today’s range of ' + formatPrice(quote.low) + ' to ' + formatPrice(quote.high)}>
    <div className="day-range-track"><i style={{ left: position + '%' }}/></div>
    <div className="mini-chart-label"><span>Day low</span><span>Current price in range</span><span>Day high</span></div>
  </div>
}

function TileRecommendation({ symbol }: { symbol: string }) {
  const ml = useMlRecommendation(currentMarketDate(), symbol, true)
  const document = ml.news.data ?? ml.insights.data ?? ml.prediction.data
  const prediction = document?.prediction
  if (ml.loading && !document) return <div className="tile-recommendation" aria-live="polite"><span className="tile-recommendation-label">Recommendation</span><span className="tile-recommendation-loading">Loading…</span></div>
  if (!prediction) return <div className="tile-recommendation tile-recommendation-empty"><span className="tile-recommendation-label">Recommendation</span><b>Not available yet</b><small>{document?.date ? `Last update · ${document.date}` : 'Research is still collecting data'}</small></div>
  const probability = prediction.probability_positive_return
  return <div className="tile-recommendation">
    <div className="tile-recommendation-head"><span className="tile-recommendation-label">Recommendation</span><span className={`signal-pill ${prediction.direction.toLowerCase()}`}>{prediction.direction}</span></div>
    <div className="tile-recommendation-probability"><strong>{(probability * 100).toFixed(1)}%</strong><span>chance of a higher close</span></div>
    <div className="tile-recommendation-bar" aria-hidden="true"><i style={{ width: `${probability * 100}%` }}/></div>
  </div>
}

function QuoteCard({ etf, apiKey }: { etf: EtfDefinition; apiKey: string }) {
  const quote = useFinnhubQuote(etf.symbol, apiKey)
  return <Link to={`/etfs/${etf.symbol}`} className="etf-card">
    <div className="etf-card-top"><div><b className="ticker">{etf.symbol}</b><span>{etf.name}</span></div><span className="fund-arrow"><ArrowUpRight size={21}/></span></div>
    {quote.loading && !quote.data ? <div className="quote-skeleton"/> : quote.error && !quote.data ? <div className="quote-error"><span>Price unavailable</span><small>Open for details</small></div> : <div className="etf-price"><strong>{formatPrice(quote.data?.current)}</strong><span className={(quote.data?.changePercent ?? 0) >= 0 ? 'positive' : 'negative'}>{formatPercent(quote.data?.changePercent)}</span></div>}
    <div className="quote-range"><span>Day range</span><b>{formatPrice(quote.data?.low)} – {formatPrice(quote.data?.high)}</b></div>
    <MiniPriceChart symbol={etf.symbol} quote={quote.data}/>
    <TileRecommendation symbol={etf.symbol}/>
    <div className="etf-card-foot"><span>{quote.cached ? 'Last available' : 'As of'} · {formatTimestamp(quote.data?.timestamp)}</span><ChevronRight size={15}/></div>
  </Link>
}

function Dashboard({ apiKey }: { apiKey: string }) {
  const sectors = ETF_UNIVERSE.filter(etf => etf.category === 'sector')
  const contexts = ETF_UNIVERSE.filter(etf => etf.category === 'context')
  return <>
    <section className="market-intro" aria-labelledby="overview-title">
      <div className="intro-meta"><span className="eyebrow">ETF RESEARCH / MARKET OVERVIEW</span><span className="session-status"><span/>{marketStatus()} · regular US hours</span></div>
      <div className="intro-main"><h1 id="overview-title">THE MARKET.<br/><span>IN PERSPECTIVE.</span></h1><div className="intro-copy"><p>A clearer view of every sector.</p><span>Explore prices, compare market context, and follow the latest research outlook.</span><a href="#sectors" className="primary-btn">EXPLORE THE SECTORS <ArrowRight size={17}/></a></div></div>
      <div className="overview-index"><div><b>01 /</b><span>Sector coverage</span><strong>{sectors.length} ETFs</strong></div><div><b>02 /</b><span>Market context</span><strong>{contexts.length} ETFs</strong></div><div><b>03 /</b><span>Research focus</span><strong>Next-close direction</strong></div></div>
    </section>
    <EtfSection id="sectors" number="01" title="Sector by sector." description="Explore the industries moving the market." items={sectors} apiKey={apiKey}/>
    <EtfSection id="context" number="02" title="The wider picture." description="Equities, bonds, and commodities in context." items={contexts} apiKey={apiKey}/>
    <div className="data-note"><Activity size={16}/><span>Quotes supplied by Finnhub. Each fund shows its latest available research; unavailable forecasts are never estimated.</span></div>
  </>
}

function EtfSection({ id, number, title, description, items, apiKey }: { id: string; number: string; title: string; description: string; items: EtfDefinition[]; apiKey: string }) {
  return <section id={id} className="etf-group"><div className="section-heading"><div><span className="section-number">{number} /</span><h2>{title}</h2><p>{description}</p></div><span className="fund-count">{items.length} FUNDS <ArrowUpRight size={16}/></span></div><div className="etf-grid">{items.map(etf => <QuoteCard key={etf.symbol} etf={etf} apiKey={apiKey}/>)}</div></section>
}

function csvDownload(symbol: string, candles: Candle[]) {
  const rows = ['timestamp,open,high,low,close,volume', ...candles.map(candle => `${new Date(candle.time * 1000).toISOString()},${candle.open},${candle.high},${candle.low},${candle.close},${candle.volume}`)]
  const blob = new Blob([rows.join('\n')], { type: 'text/csv' })
  const url = URL.createObjectURL(blob)
  const anchor = document.createElement('a')
  anchor.href = url
  anchor.download = `${symbol}-finnhub-bars.csv`
  anchor.click()
  URL.revokeObjectURL(url)
}

const easternClock = new Intl.DateTimeFormat('en-CA', {
  timeZone: 'America/New_York',
  year: 'numeric',
  month: '2-digit',
  day: '2-digit',
  hour: '2-digit',
  minute: '2-digit',
  hourCycle: 'h23',
})

function visibleCandles(candles: Candle[], range: ChartRange, extendedHours: boolean) {
  let visible = candles
  if (!extendedHours && (range === '1D' || range === '5D' || range === '1M' || range === '3M')) {
    visible = candles.filter(candle => {
      const parts = Object.fromEntries(easternClock.formatToParts(candle.time * 1000).map(part => [part.type, part.value]))
      const minutes = Number(parts.hour) * 60 + Number(parts.minute)
      return minutes >= 9 * 60 + 30 && minutes <= 16 * 60
    })
  }
  if (range !== '1D' && range !== '5D') return visible
  const tradingDates = [...new Set(visible.map(candle => easternClock.format(candle.time * 1000).slice(0, 10)))].slice(range === '1D' ? -1 : -5)
  const allowedDates = new Set(tradingDates)
  return visible.filter(candle => allowedDates.has(easternClock.format(candle.time * 1000).slice(0, 10)))
}

function currentMarketDate() {
  const parts = Object.fromEntries(easternClock.formatToParts(new Date()).map(part => [part.type, part.value]))
  return `${parts.year}-${parts.month}-${parts.day}`
}

type BatchInferenceStatus = {
  tone: 'checking' | 'available' | 'gated' | 'unavailable'
  label: string
}

function batchInferenceStatus(state: ReturnType<typeof useMlRecommendation>): BatchInferenceStatus {
  const document = state.news.data ?? state.prediction.data
  if (state.loading) return { tone: 'checking', label: 'Checking' }
  if (!document) return { tone: 'unavailable', label: 'No update available' }
  if (document.prediction) return { tone: 'available', label: `${document.prediction.direction} · ${document.date}` }
  if (state.prediction.error?.code === 'NO_VALIDATED_MODEL') {
    return { tone: 'gated', label: `Forecast not yet available · ${document.date}` }
  }
  return { tone: 'gated', label: `Updated · ${document.date}` }
}

function ETFDetail({ apiKey }: { apiKey: string }) {
  const symbol = window.location.pathname.split('/')[2]?.toUpperCase() ?? ''
  const definition = findEtf(symbol)
  const [range, setRange] = useState<ChartRange>('1D')
  const [style, setStyle] = useState<ChartStyle>('area')
  const [extendedHours, setExtendedHours] = useState(false)
  const [benchmark, setBenchmark] = useState(symbol === 'SPY' ? '' : 'SPY')
  const [activeIndicators, setActiveIndicators] = useState<Set<IndicatorKey>>(new Set(['sma20']))
  const quote = useFinnhubQuote(symbol, apiKey)
  const candles = useTwelveDataCandles(symbol, range, TWELVE_DATA_API_KEY)
  const profile = useFinnhubProfile(symbol, apiKey)
  const comparison = useTwelveDataCandles(benchmark, range, TWELVE_DATA_API_KEY)
  const batchInference = useMlRecommendation(currentMarketDate(), symbol, true)
  const batchStatus = batchInferenceStatus(batchInference)
  const rangeRequest = twelveDataRangeRequest(range)
  const displayedCandles = useMemo(() => visibleCandles(candles.data ?? [], range, extendedHours), [candles.data, range, extendedHours])
  const displayedComparison = useMemo(() => visibleCandles(comparison.data ?? [], range, extendedHours), [comparison.data, range, extendedHours])
  const indicators = useIndicators(displayedCandles)
  const statistics = useMemo(() => calculateStatistics(displayedCandles, rangeRequest.interval === '1day' ? 252 : rangeRequest.interval === '1week' ? 52 : rangeRequest.interval === '1month' ? 12 : 98_280), [displayedCandles, rangeRequest.interval])
  if (!definition) return <FinnhubErrorState error={new FinnhubError(`${symbol} is not in the configured ETF universe.`, 'NO_DATA')}/>

  const toggleIndicator = (key: IndicatorKey) => {
    setActiveIndicators(current => {
      const next = new Set(current)
      if (next.has(key)) next.delete(key)
      else {
        const definition = INDICATORS.find(item => item.key === key)
        const overlayCount = INDICATORS.filter(item => item.kind === 'overlay' && next.has(item.key)).length
        if (definition?.kind === 'overlay' && overlayCount >= 3) return current
        next.add(key)
      }
      return next
    })
  }

  return <>
    <Topbar eyebrow={`FUND OVERVIEW · ${marketStatus().toUpperCase()}`} title={`${symbol} · ${profile.data?.name ?? definition.name}`}>
      <Link to="/" className="outline-btn">← Dashboard</Link>
      <button className="outline-btn" disabled={!candles.data?.length} onClick={() => csvDownload(symbol, candles.data ?? [])}><Download size={14}/> Download prices</button>
      <button className="outline-btn" onClick={() => { quote.refresh(); candles.refresh(); profile.refresh(); comparison.refresh() }}><RefreshCw size={14}/> Refresh</button>
    </Topbar>

    <section className="panel finnhub-viewer">
      <div className="viewer-header">
        <div className="quote-identity">
          {profile.data?.logo && <img src={profile.data.logo} alt="" className="etf-logo"/>}
          <div><div className="eyebrow">{profile.data?.exchange ?? 'US ETF'} · {profile.data?.currency ?? 'USD'}</div><h2>{definition.name}</h2><div className="quote-line"><strong>{formatPrice(quote.data?.current ?? candles.data?.at(-1)?.close)}</strong>{quote.data && <span className={quote.data.change >= 0 ? 'positive' : 'negative'}>{quote.data.change >= 0 ? '+' : ''}{quote.data.change.toFixed(2)} ({formatPercent(quote.data.changePercent)})</span>}</div><small>{marketStatus()} · quote {formatTimestamp(quote.data?.timestamp)}</small></div>
        </div>
        <div className="viewer-header-actions">
          <div className={`batch-status status-${batchStatus.tone}`} title={`Research outlook: ${batchStatus.label}`} aria-live="polite"><Database size={13}/><span>Research outlook · {batchStatus.label}</span></div>
          <div className="range-tabs large" aria-label="Chart range">{RANGES.map(option => <button key={option} className={range === option ? 'active' : ''} aria-pressed={range === option} onClick={() => setRange(option)}>{option}</button>)}</div>
        </div>
      </div>

      <div className="quote-stat-grid">
        <Metric label="Open" value={formatPrice(quote.data?.open)}/>
        <Metric label="Previous close" value={formatPrice(quote.data?.previousClose)}/>
        <Metric label="Day high" value={formatPrice(quote.data?.high)}/>
        <Metric label="Day low" value={formatPrice(quote.data?.low)}/>
        <Metric label="Period return" value={formatPercent(statistics?.returnPercent)} tone={(statistics?.returnPercent ?? 0) >= 0 ? 'positive' : 'negative'}/>
        <Metric label="Largest decline" value={formatPercent(statistics?.maximumDrawdown)} tone="negative"/>
      </div>

      <div className="chart-toolbar">
        <div className="segmented-control"><button className={style === 'area' ? 'active' : ''} aria-pressed={style === 'area'} onClick={() => setStyle('area')}>Area</button><button className={style === 'candles' ? 'active' : ''} aria-pressed={style === 'candles'} onClick={() => setStyle('candles')}>Candles</button></div>
        <div className="indicator-controls">{INDICATORS.map(indicator => <button key={indicator.key} className={activeIndicators.has(indicator.key) ? 'active' : ''} aria-pressed={activeIndicators.has(indicator.key)} onClick={() => toggleIndicator(indicator.key)}>{indicator.label}</button>)}</div>
        {(range === '1D' || range === '5D' || range === '1M' || range === '3M') && <label className="extended-hours-control"><input type="checkbox" checked={extendedHours} onChange={event => setExtendedHours(event.target.checked)}/><span>Extended hours</span></label>}
        <label className="benchmark-control"><span>Compare</span><select value={benchmark} onChange={event => setBenchmark(event.target.value)}><option value="">None</option>{ETF_UNIVERSE.filter(etf => etf.symbol !== symbol).map(etf => <option value={etf.symbol} key={etf.symbol}>{etf.symbol}</option>)}</select></label>
      </div>

      {candles.loading && !candles.data ? <div className="chart-placeholder large"/> : candles.error && !candles.data ? <FinnhubErrorState error={candles.error} retry={candles.refresh} provider="Twelve Data"/> : !displayedCandles.length ? <div className="panel empty-state"><BarChart3 size={20}/><div><h2>No price history available</h2><p>There are no prices for {symbol} in this period. Try another date range.</p></div></div> : <FinnhubChart symbol={symbol} candles={displayedCandles} benchmarkSymbol={benchmark} benchmark={displayedComparison} style={style} indicators={indicators} activeIndicators={activeIndicators}/>}
      {candles.error && candles.data && <div className="inline-warning"><AlertTriangle size={14}/> Prices could not be updated. Showing the last available history.</div>}
    </section>

    <section className="research-grid">
      <div className="panel stats-panel"><div className="panel-head"><div><div className="eyebrow">SELECTED RANGE</div><h2>Risk and performance</h2></div></div><div className="research-metrics">
        <Metric label="Annualized volatility" value={formatPercent(statistics?.annualizedVolatility)}/>
        <Metric label="Periods with a gain" value={formatPercent(statistics?.positiveSessions)}/>
        <Metric label="Period high" value={formatPrice(statistics?.periodHigh)}/>
        <Metric label="Period low" value={formatPrice(statistics?.periodLow)}/>
        <Metric label="Average volume" value={formatNumber(statistics?.averageVolume)}/>
        <Metric label="Price observations" value={formatNumber(displayedCandles.length)}/>
      </div></div>
    </section>
  </>
}

function Metric({ label, value, tone = '' }: { label: string; value: string; tone?: string }) {
  return <div className="metric"><span>{label}</span><b className={tone}>{value}</b></div>
}

// Kept exportable for backwards-compatible embeds; the dashboard is the only
// navigation surface and owns recommendations in each ETF tile.
export function Recommendation() {
  const [symbol, setSymbol] = useState('XLK')
  const [showAllArticles, setShowAllArticles] = useState(false)
  const [date, setDate] = useState(currentMarketDate())
  const [latest, setLatest] = useState(true)
  const ml = useMlRecommendation(date, symbol, latest)
  const document = ml.news.data ?? ml.insights.data ?? ml.prediction.data
  const prediction = ml.prediction.data?.prediction
  const error = ml.prediction.error ?? ml.news.error ?? ml.insights.error
  const provisional = document?.run_kind === 'intraday'

  return <>
    <Topbar eyebrow="EXPLORE THE OUTLOOK" title="Recommendations">
      <label className="date-control"><span>Fund</span><select value={symbol} onChange={event => setSymbol(event.target.value)}>{ETF_UNIVERSE.map(etf => <option key={etf.symbol} value={etf.symbol}>{etf.symbol} · {etf.name}</option>)}</select></label>
      <label className="date-control"><span>Show</span><select value={latest ? 'latest' : 'date'} onChange={event => { setLatest(event.target.value === 'latest'); setDate(currentMarketDate()) }}><option value="latest">Latest available</option><option value="date">Choose a date</option></select></label>
      {!latest && <label className="date-control"><span>Research date</span><input type="date" max={currentMarketDate()} value={date} onChange={event => setDate(event.target.value)}/></label>}
      <button className="outline-btn" disabled={ml.loading} onClick={ml.refresh}><RefreshCw size={14}/>{ml.loading ? 'Updating…' : 'Refresh'}</button>
    </Topbar>
    <div className="notice-banner live-source"><BookOpen size={20}/><div><b>A research outlook for each fund</b><span>Explore the outlook for the next trading day alongside available news. </span></div></div>
    <div aria-live="polite" aria-atomic="true">
      {ml.loading ? <div className="panel empty-state"><RefreshCw size={20}/><p>Loading research for {symbol}…</p></div> : null}
    </div>
    {!ml.loading && (error && !document ? <MlErrorState error={error} retry={ml.refresh}/> : <>
      <div className="research-update">
        <span>Research date: <b>{document?.date ?? date}</b></span>
        {document?.generated_at && <span>Updated <time dateTime={document.generated_at}>{new Date(document.generated_at).toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' })}</time></span>}
        <span>{provisional ? 'During-market update' : 'After-market update'}</span>
      </div>
      <section className="recommendation-layout" aria-label={`${symbol} research`}>
        <div className="panel insight-panel">
          <div className="panel-head"><div><div className="eyebrow">{symbol} · {findEtf(symbol)?.name}</div><h2>Next trading day outlook</h2></div>{prediction && <span className={`signal-pill ${prediction.direction.toLowerCase()}`}>{prediction.direction}</span>}</div>
          {prediction ? <>
            <div className="insight-primary"><strong>{(prediction.probability_positive_return * 100).toFixed(1)}%</strong><span>estimated chance of a higher closing price</span></div>
            <div className="confidence-track" aria-hidden="true"><i style={{ width: `${prediction.probability_positive_return * 100}%` }}/></div>
            <p className="outlook-explanation">{prediction.direction === 'Bullish' ? 'The forecast leans toward a price increase.' : prediction.direction === 'Bearish' ? 'The forecast leans toward a price decrease.' : 'The forecast does not favor a clear direction.'}</p>
          </> : <div className="empty-state"><BookOpen size={22}/><div><h2>Forecast not yet available</h2><p>{error ? formatMlError(error) : 'There is not enough information to offer an outlook for this date.'}</p><Link to={`/etfs/${symbol}`} className="outline-btn">Explore {symbol} prices <ChevronRight size={14}/></Link></div></div>}
        </div>
        <div className="panel news-panel">
          <div className="panel-head"><div><div className="eyebrow">IN THE NEWS</div><h2>Related articles</h2></div><span>{document?.articles.length ?? 0} articles</span></div>
          {document?.articles.length ? <div className="news-list">{document.articles.slice(0, showAllArticles ? undefined : 8).map(article => <article className="news-item" key={article.article_id}>
            <div>{article.url && /^https?:\/\//i.test(article.url) ? <a href={article.url} target="_blank" rel="noopener noreferrer"><b>{article.headline}</b><span className="sr-only"> (opens in a new tab)</span></a> : <b>{article.headline}</b>}
              <small>{article.source} · <time dateTime={article.published_at}>{new Date(article.published_at).toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' })}</time></small>
            </div>
            <span className={article.sentiment_score > 0.1 ? 'positive' : article.sentiment_score < -0.1 ? 'negative' : ''}>{article.sentiment_score > 0.1 ? 'Positive tone' : article.sentiment_score < -0.1 ? 'Negative tone' : 'Mixed tone'}</span>
          </article>)}</div> : <div className="empty-state"><BookOpen size={22}/><div><h2>{symbol === 'XLK' ? 'No articles available' : 'News coverage is coming soon'}</h2><p>{symbol === 'XLK' ? 'No news articles are available in this update. Check back later for more coverage.' : 'Related news is currently available for the technology fund (XLK). Other funds will be added as coverage expands.'}</p></div></div>}
          {(document?.articles.length ?? 0) > 8 && <button className="outline-btn" aria-expanded={showAllArticles} onClick={() => setShowAllArticles(value => !value)}>{showAllArticles ? 'Show fewer articles' : `Show all ${document?.articles.length} articles`}</button>}
          {!!document?.articles.length && <p className="news-explanation">Article tone describes the language in the news; it does not predict price movement.</p>}
        </div>
      </section>
    </>)}
  </>
}

function formatMlError(error: MlApiError) {
  switch (error.code) {
    case 'NO_VALIDATED_MODEL': return 'A forecast is not available for this fund yet. You can still explore its prices and available news.'
    case 'FEATURES_UNAVAILABLE': return 'No research is available for this date. Choose another trading day or select “Latest available.”'
    case 'INVALID_DATE': return 'Choose a valid research date using the date selector.'
    case 'NETWORK_ERROR': return 'We could not load the latest research. Check your connection and try again.'
    default: return 'Research is temporarily unavailable. Please try again shortly.'
  }
}

function MlErrorState({ error, retry }: { error: MlApiError; retry?: () => void }) {
  const title = error.code === 'FEATURES_UNAVAILABLE' ? 'No research for this date' : error.code === 'INVALID_DATE' ? 'Choose a research date' : 'Research is temporarily unavailable'
  return <div role="status" className="panel empty-state finnhub-error"><WifiOff size={22}/><div><h2>{title}</h2><p>{formatMlError(error)}</p>{retry && <button className="outline-btn" onClick={retry}><RefreshCw size={14}/> Try again</button>}</div></div>
}

function RouterView({ apiKey }: { apiKey: string }) {
  const path = window.location.pathname
  if (path.startsWith('/etfs/')) return <ETFDetail apiKey={apiKey}/>
  return <Dashboard apiKey={apiKey}/>
}

export default function App() {
  const apiKey = configuredFinnhubKey()
  return <Shell>
    <RouterView apiKey={apiKey}/>
  </Shell>
}
