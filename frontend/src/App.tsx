import { useMemo, useState } from 'react'
import {
  Activity,
  AlertTriangle,
  BarChart3,
  CheckCircle2,
  ChevronRight,
  Database,
  Download,
  Gauge,
  GitBranch,
  KeyRound,
  LayoutDashboard,
  RefreshCw,
  Settings,
  ShieldCheck,
  WifiOff,
} from 'lucide-react'
import FinnhubChart from './components/FinnhubChart'
import { ETF_UNIVERSE, findEtf, type EtfDefinition } from './etfUniverse'
import { FinnhubError, getQuote } from './finnhub/client'
import { calculateStatistics } from './finnhub/indicators'
import { clearFinnhubKey, configuredFinnhubKey, saveFinnhubKey } from './finnhub/key'
import { useFinnhubEtfProfile, useFinnhubProfile, useFinnhubQuote, useIndicators } from './finnhub/hooks'
import type { Candle, ChartRange, ChartStyle, IndicatorKey } from './finnhub/types'
import { useLiveMarket } from './liveMarket/hooks'
import type { ConnectionStatus, LiveTrade } from './liveMarket/types'
import { TwelveDataError } from './twelveData/client'
import { useTwelveDataCandles } from './twelveData/hooks'
import { twelveDataRangeRequest } from './twelveData/ranges'

const TWELVE_DATA_API_KEY = (import.meta.env.VITE_TWELVE_DATA_API_KEY ?? '').trim()

const DISCLAIMER = 'Research data only. Not investment advice. No trades are executed by this platform.'
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
  const active = window.location.pathname === to || (to !== '/' && window.location.pathname.startsWith(to))
  return <a className={active ? 'active' : ''} href={to}>{children}</a>
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

function liveStatusLabel(status: ConnectionStatus, hasTrade: boolean) {
  if (status === 'connecting') return 'Connecting'
  if (status === 'reconnecting') return 'Reconnecting'
  if (status === 'offline') return 'Offline'
  return hasTrade ? 'Live' : 'Connected · waiting for data'
}

function hasRecentLiveTrade(trade: LiveTrade | null) {
  if (!trade) return false
  const timestamp = Date.parse(trade.timestamp)
  return Number.isFinite(timestamp) && Date.now() - timestamp <= 120_000
}

function Shell({ children, onConfigure }: { children: React.ReactNode; onConfigure: () => void }) {
  return <div className="app-shell">
    <header>
      <Link to="/" className="brand"><span className="brand-mark">A</span><span>ARRAKIS <em>/ FINNHUB ETF RESEARCH</em></span></Link>
      <nav className="top-nav" aria-label="Primary navigation">
        <NavLink to="/"><LayoutDashboard size={16}/><span>ETF dashboard</span></NavLink>
        <NavLink to="/health"><Gauge size={16}/><span>Data status</span></NavLink>
        <NavLink to="/recommendation"><GitBranch size={16}/><span>Recommendations</span></NavLink>
      </nav>
      <button className="header-config" onClick={onConfigure}><Settings size={15}/> Finnhub key</button>
    </header>
    <main>{children}</main>
    <div className="disclaimer"><AlertTriangle size={13}/> {DISCLAIMER}</div>
  </div>
}

function Topbar({ eyebrow, title, children }: { eyebrow: string; title: string; children?: React.ReactNode }) {
  return <div className="topbar"><div><div className="eyebrow">{eyebrow}</div><h1>{title}</h1></div><div className="topbar-actions">{children}</div></div>
}

function FinnhubErrorState({ error, retry, compact = false, provider = 'Finnhub' }: { error: FinnhubError | TwelveDataError; retry?: () => void; compact?: boolean; provider?: string }) {
  const title = error.code === 'RATE_LIMITED'
    ? `${provider} rate limit reached`
    : error.code === 'ENTITLEMENT'
      ? `${provider} plan does not include this dataset`
      : error.code === 'NO_DATA'
        ? `No ${provider} data`
        : `${provider} request unavailable`
  return <div className={`panel empty-state finnhub-error ${compact ? 'compact' : ''}`}>
    <WifiOff size={20}/>
    <div><h2>{title}</h2><p>{error.message}</p><small className="mono">{error.code}{error.status ? ` · HTTP ${error.status}` : ''}</small>{retry && <button className="outline-btn" onClick={retry}><RefreshCw size={14}/> Retry</button>}</div>
  </div>
}

function KeySetup({ initialKey, onReady, onCancel }: { initialKey: string; onReady: (key: string) => void; onCancel?: () => void }) {
  const [value, setValue] = useState(initialKey)
  const [checking, setChecking] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const validate = async () => {
    const normalized = value.trim()
    if (!normalized) {
      setError('Enter a Finnhub API key.')
      return
    }
    setChecking(true)
    setError(null)
    const controller = new AbortController()
    const timeout = window.setTimeout(() => controller.abort(), 10_000)
    try {
      await getQuote('XLK', normalized, controller.signal)
      saveFinnhubKey(normalized)
      onReady(normalized)
    } catch (requestError) {
      setError(requestError instanceof Error ? requestError.message : 'Finnhub validation failed.')
    } finally {
      window.clearTimeout(timeout)
      setChecking(false)
    }
  }

  return <div className={onCancel ? 'modal-backdrop' : 'key-gate'}>
    <section className="panel key-panel">
      <KeyRound size={26}/>
      <div className="eyebrow">DIRECT FINNHUB CONNECTION</div>
      <h2>Connect ETF market data</h2>
      <p>The browser will call Finnhub directly. The key is held in session storage and is cleared when the browser session ends. It is never sent to Supabase or the Arrakis backend.</p>
      <label><span>Finnhub API key</span><input type="password" autoComplete="off" value={value} onChange={event => setValue(event.target.value)} placeholder="Paste your Finnhub key"/></label>
      {error && <div className="key-error">{error}</div>}
      <div className="dialog-actions">
        {onCancel && <button className="outline-btn" onClick={onCancel}>Cancel</button>}
        <button className="primary-btn" disabled={checking} onClick={validate}>{checking ? 'Checking Finnhub…' : 'Validate and continue'}</button>
      </div>
      <small>Historical stock candles may require Finnhub premium market-data access.</small>
    </section>
  </div>
}

function QuoteCard({ etf, apiKey }: { etf: EtfDefinition; apiKey: string }) {
  const quote = useFinnhubQuote(etf.symbol, apiKey)
  return <Link to={`/etfs/${etf.symbol}`} className="etf-card">
    <div className="etf-card-top"><div><b className="ticker">{etf.symbol}</b><span>{etf.name}</span></div><span className={`category-tag ${etf.category}`}>{etf.category}</span></div>
    {quote.loading && !quote.data ? <div className="quote-skeleton"/> : quote.error && !quote.data ? <div className="quote-error"><span>{quote.error.code}</span><small>Open for details</small></div> : <div className="etf-price"><strong>{formatPrice(quote.data?.current)}</strong><span className={(quote.data?.changePercent ?? 0) >= 0 ? 'positive' : 'negative'}>{formatPercent(quote.data?.changePercent)}</span></div>}
    <div className="quote-range"><span>Day range</span><b>{formatPrice(quote.data?.low)} – {formatPrice(quote.data?.high)}</b></div>
    <div className="etf-card-foot"><span>{quote.cached ? 'Cached Finnhub quote' : 'Finnhub quote'} · {formatTimestamp(quote.data?.timestamp)}</span><ChevronRight size={15}/></div>
  </Link>
}

function Dashboard({ apiKey }: { apiKey: string }) {
  const sectors = ETF_UNIVERSE.filter(etf => etf.category === 'sector')
  const contexts = ETF_UNIVERSE.filter(etf => etf.category === 'context')
  return <>
    <Topbar eyebrow={`DIRECT FINNHUB REST · ${marketStatus().toUpperCase()}`} title="ETF research dashboard"/>
    <div className="notice-banner live-source"><Activity size={17}/><div><b>Frontend-owned Finnhub data</b><span>Quotes are requested directly from Finnhub with a three-request concurrency limit. No database or Arrakis market API is used.</span></div></div>
    <EtfSection title="Sector ETFs" items={sectors} apiKey={apiKey}/>
    <EtfSection title="Market context" items={contexts} apiKey={apiKey}/>
  </>
}

function EtfSection({ title, items, apiKey }: { title: string; items: EtfDefinition[]; apiKey: string }) {
  return <section className="etf-group"><div className="section-heading"><div><div className="eyebrow">{title.toUpperCase()}</div><h2>{items.length} instruments</h2></div></div><div className="etf-grid">{items.map(etf => <QuoteCard key={etf.symbol} etf={etf} apiKey={apiKey}/>)}</div></section>
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
  const etfProfile = useFinnhubEtfProfile(symbol, apiKey)
  const comparison = useTwelveDataCandles(benchmark, range, TWELVE_DATA_API_KEY)
  const liveMarket = useLiveMarket(definition?.symbol ?? '')
  const rangeRequest = twelveDataRangeRequest(range)
  const displayedCandles = useMemo(() => visibleCandles(candles.data ?? [], range, extendedHours), [candles.data, range, extendedHours])
  const displayedComparison = useMemo(() => visibleCandles(comparison.data ?? [], range, extendedHours), [comparison.data, range, extendedHours])
  const indicators = useIndicators(displayedCandles)
  const statistics = useMemo(() => calculateStatistics(displayedCandles, rangeRequest.interval === '1day' ? 252 : rangeRequest.interval === '1week' ? 52 : rangeRequest.interval === '1month' ? 12 : 98_280), [displayedCandles, rangeRequest.interval])
  const holdings = etfProfile.data?.holdings ?? []
  const sectorExposure = Object.entries(etfProfile.data?.sectorExposure ?? {}).sort((left, right) => right[1] - left[1])

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
    <Topbar eyebrow={`FINNHUB ETF VIEW · ${marketStatus().toUpperCase()}`} title={`${symbol} · ${profile.data?.name ?? definition.name}`}>
      <Link to="/" className="outline-btn">← Dashboard</Link>
      <button className="outline-btn" disabled={!candles.data?.length} onClick={() => csvDownload(symbol, candles.data ?? [])}><Download size={14}/> CSV</button>
      <button className="outline-btn" onClick={() => { quote.refresh(); candles.refresh(); profile.refresh(); etfProfile.refresh(); comparison.refresh() }}><RefreshCw size={14}/> Refresh</button>
    </Topbar>
    <div className="notice-banner live-source"><Database size={17}/><div><b>Twelve Data chart history</b><span>{candles.cached ? `Browser cache${candles.stale ? ' · stale after refresh failure' : ''}` : 'Fresh Twelve Data response'} · {rangeRequest.label} history. Arrakis market-api WebSocket runs independently and feeds the backend inference pipeline; it does not source this chart.</span></div><span className="chart-source">REST</span></div>

    <section className="panel finnhub-viewer">
      <div className="viewer-header">
        <div className="quote-identity">
          {profile.data?.logo && <img src={profile.data.logo} alt="" className="etf-logo"/>}
          <div><div className="eyebrow">{profile.data?.exchange ?? 'US ETF'} · {profile.data?.currency ?? 'USD'}</div><h2>{definition.name}</h2><div className="quote-line"><strong>{formatPrice(quote.data?.current ?? candles.data?.at(-1)?.close)}</strong>{quote.data && <span className={quote.data.change >= 0 ? 'positive' : 'negative'}>{quote.data.change >= 0 ? '+' : ''}{quote.data.change.toFixed(2)} ({formatPercent(quote.data.changePercent)})</span>}</div><small>{marketStatus()} · quote {formatTimestamp(quote.data?.timestamp)}</small></div>
        </div>
        <div className="viewer-header-actions">
          <div className={`live-status status-${liveMarket.status}`} title={`Backend inference feed (market-api WebSocket): ${liveStatusLabel(liveMarket.status, hasRecentLiveTrade(liveMarket.lastTrade))}`} aria-live="polite"><WifiOff size={13}/><span>Inference feed · {liveStatusLabel(liveMarket.status, hasRecentLiveTrade(liveMarket.lastTrade))}</span></div>
          <div className="range-tabs large" aria-label="Chart range">{RANGES.map(option => <button key={option} className={range === option ? 'active' : ''} onClick={() => setRange(option)}>{option}</button>)}</div>
        </div>
      </div>

      <div className="quote-stat-grid">
        <Metric label="Open" value={formatPrice(quote.data?.open)}/>
        <Metric label="Previous close" value={formatPrice(quote.data?.previousClose)}/>
        <Metric label="Day high" value={formatPrice(quote.data?.high)}/>
        <Metric label="Day low" value={formatPrice(quote.data?.low)}/>
        <Metric label="Period return" value={formatPercent(statistics?.returnPercent)} tone={(statistics?.returnPercent ?? 0) >= 0 ? 'positive' : 'negative'}/>
        <Metric label="Max drawdown" value={formatPercent(statistics?.maximumDrawdown)} tone="negative"/>
      </div>

      <div className="chart-toolbar">
        <div className="segmented-control"><button className={style === 'area' ? 'active' : ''} onClick={() => setStyle('area')}>Area</button><button className={style === 'candles' ? 'active' : ''} onClick={() => setStyle('candles')}>Candles</button></div>
        <div className="indicator-controls">{INDICATORS.map(indicator => <button key={indicator.key} className={activeIndicators.has(indicator.key) ? 'active' : ''} onClick={() => toggleIndicator(indicator.key)}>{indicator.label}</button>)}</div>
        {(range === '1D' || range === '5D' || range === '1M' || range === '3M') && <label className="extended-hours-control"><input type="checkbox" checked={extendedHours} onChange={event => setExtendedHours(event.target.checked)}/><span>Extended hours</span></label>}
        <label className="benchmark-control"><span>Compare</span><select value={benchmark} onChange={event => setBenchmark(event.target.value)}><option value="">None</option>{ETF_UNIVERSE.filter(etf => etf.symbol !== symbol).map(etf => <option value={etf.symbol} key={etf.symbol}>{etf.symbol}</option>)}</select></label>
      </div>

      {candles.loading && !candles.data ? <div className="chart-placeholder large"/> : candles.error && !candles.data ? <FinnhubErrorState error={candles.error} retry={candles.refresh} provider="Twelve Data"/> : !displayedCandles.length ? <div className="panel empty-state"><BarChart3 size={20}/><div><h2>No candles returned</h2><p>Twelve Data returned no OHLCV candles for {symbol} and the selected {range} range.</p></div></div> : <FinnhubChart symbol={symbol} candles={displayedCandles} benchmarkSymbol={benchmark} benchmark={displayedComparison} style={style} indicators={indicators} activeIndicators={activeIndicators}/>}
      {candles.error && candles.data && <div className="inline-warning"><AlertTriangle size={14}/> Refresh failed; cached candles remain displayed. {candles.error.message}</div>}
    </section>

    <section className="research-grid">
      <div className="panel stats-panel"><div className="panel-head"><div><div className="eyebrow">SELECTED RANGE</div><h2>Risk and performance</h2></div></div><div className="research-metrics">
        <Metric label="Annualized volatility" value={formatPercent(statistics?.annualizedVolatility)}/>
        <Metric label="Positive bars" value={formatPercent(statistics?.positiveSessions)}/>
        <Metric label="Period high" value={formatPrice(statistics?.periodHigh)}/>
        <Metric label="Period low" value={formatPrice(statistics?.periodLow)}/>
        <Metric label="Average volume" value={formatNumber(statistics?.averageVolume)}/>
        <Metric label="Displayed bars" value={formatNumber(displayedCandles.length)}/>
      </div></div>
      <div className="panel profile-panel"><div className="panel-head"><div><div className="eyebrow">FINNHUB ETF PROFILE</div><h2>Fund information</h2></div></div>{etfProfile.error && !etfProfile.data ? <EntitlementNote error={etfProfile.error}/> : <div className="research-metrics">
        <Metric label="Asset class" value={etfProfile.data?.assetClass ?? '—'}/>
        <Metric label="Expense ratio" value={etfProfile.data?.expenseRatio == null ? '—' : `${etfProfile.data.expenseRatio}%`}/>
        <Metric label="AUM" value={etfProfile.data?.aum == null ? '—' : `$${formatNumber(etfProfile.data.aum)}`}/>
        <Metric label="NAV" value={formatPrice(etfProfile.data?.nav)}/>
        <Metric label="Inception" value={etfProfile.data?.inceptionDate ?? '—'}/>
        <Metric label="ISIN" value={etfProfile.data?.isin ?? '—'}/>
      </div>}</div>
      <div className="panel holdings-panel"><div className="panel-head"><div><div className="eyebrow">COMPOSITION</div><h2>Top holdings</h2></div><span>{holdings.length} returned</span></div>{holdings.length ? <div className="holding-list">{holdings.slice(0, 10).map((holding, index) => <div key={`${holding.symbol ?? holding.name}-${index}`}><span><b>{holding.symbol ?? '—'}</b>{holding.name}</span><strong>{holding.percent == null ? '—' : `${holding.percent.toFixed(2)}%`}</strong></div>)}</div> : <EntitlementNote error={etfProfile.error}/>}</div>
      <div className="panel exposure-panel"><div className="panel-head"><div><div className="eyebrow">COMPOSITION</div><h2>Sector exposure</h2></div></div>{sectorExposure.length ? <div className="exposure-list">{sectorExposure.slice(0, 10).map(([sector, exposure]) => <div key={sector}><span>{sector}</span><div><i style={{ width: `${Math.min(100, exposure)}%` }}/></div><b>{exposure.toFixed(1)}%</b></div>)}</div> : <EntitlementNote error={etfProfile.error}/>}</div>
    </section>
  </>
}

function Metric({ label, value, tone = '' }: { label: string; value: string; tone?: string }) {
  return <div className="metric"><span>{label}</span><b className={tone}>{value}</b></div>
}

function EntitlementNote({ error }: { error?: FinnhubError | null }) {
  return <div className="entitlement-note"><ShieldCheck size={18}/><div><b>{error?.code === 'ENTITLEMENT' ? 'Finnhub entitlement required' : 'No composition returned'}</b><p>{error?.message ?? 'This Finnhub response did not include ETF composition data.'}</p></div></div>
}

function DataStatus({ apiKey }: { apiKey: string }) {
  const quote = useFinnhubQuote('XLK', apiKey)
  const candles = useTwelveDataCandles('XLK', '1D', TWELVE_DATA_API_KEY)
  return <>
    <Topbar eyebrow="FRONTEND DATA DIAGNOSTICS" title="Data connection status"><button className="outline-btn" onClick={() => { quote.refresh(); candles.refresh() }}><RefreshCw size={14}/> Run checks</button></Topbar>
    <div className="health-grid">
      <StatusCard label="Finnhub API key" value={apiKey ? 'configured' : 'missing'} good={Boolean(apiKey)}/>
      <StatusCard label="Twelve Data API key" value={TWELVE_DATA_API_KEY ? 'configured' : 'missing'} good={Boolean(TWELVE_DATA_API_KEY)}/>
      <StatusCard label="XLK quote" value={quote.loading ? 'checking' : quote.error ? quote.error.code : 'available'} good={Boolean(quote.data)}/>
      <StatusCard label="Stock candles" value={candles.loading ? 'checking' : candles.error ? candles.error.code : candles.data?.length ? `${candles.data.length} bars` : 'no_data'} good={Boolean(candles.data?.length)}/>
      <StatusCard label="Market state" value={marketStatus()} good/>
    </div>
    <div className="panel operational-note"><ShieldCheck size={20}/><div><h2>Verified data boundary</h2><p>ETF quotes and profile/holdings calls are issued directly to finnhub.io. Historical chart candles are issued directly to api.twelvedata.com. IndexedDB is used only as a browser cache. The Arrakis market-api WebSocket runs independently and feeds the backend inference pipeline; the frontend chart does not use database-backed bar REST endpoints.</p></div></div>
    {quote.error && <FinnhubErrorState error={quote.error} retry={quote.refresh}/>}
    {candles.error && <FinnhubErrorState error={candles.error} retry={candles.refresh} provider="Twelve Data"/>}
  </>
}

function StatusCard({ label, value, good }: { label: string; value: string; good: boolean }) {
  return <div className="panel health-metric"><span className="eyebrow">{label}</span><strong className={good ? 'good' : 'warn'}>{value}</strong><small>Direct browser check</small></div>
}

function Recommendation() {
  return <><Topbar eyebrow="RESEARCH / MODEL INSIGHTS" title="Recommendations"/><div className="panel recommendation-page"><CheckCircle2 size={28}/><div><div className="eyebrow">SEPARATE ML BOUNDARY</div><h2>Market charts and the inference pipeline use separate data paths</h2><p>The recommendation engine remains a separate research system. This page intentionally does not infer a recommendation from chart indicators or call database-backed market-data endpoints.</p><div className="recommendation-contract"><span>Market display source</span><code>Twelve Data REST history + Finnhub quote</code><span>Backend inference feed</span><code>Arrakis market-api WebSocket (Finnhub via Kafka), independent of the chart</code><span>Recommendation source</span><code>Not connected in this frontend market view</code></div></div></div></>
}

function RouterView({ apiKey }: { apiKey: string }) {
  const path = window.location.pathname
  if (path.startsWith('/etfs/')) return <ETFDetail apiKey={apiKey}/>
  if (path === '/health') return <DataStatus apiKey={apiKey}/>
  if (path === '/recommendation') return <Recommendation/>
  return <Dashboard apiKey={apiKey}/>
}

export default function App() {
  const [apiKey, setApiKey] = useState(configuredFinnhubKey)
  const [configuring, setConfiguring] = useState(false)
  if (!apiKey) return <KeySetup initialKey="" onReady={setApiKey}/>
  return <Shell onConfigure={() => setConfiguring(true)}>
    <RouterView apiKey={apiKey}/>
    {configuring && <KeySetup initialKey={apiKey} onReady={key => { setApiKey(key); setConfiguring(false) }} onCancel={() => setConfiguring(false)}/>}
    <button className="clear-key" onClick={() => { clearFinnhubKey(); setApiKey('') }}>Clear session key</button>
  </Shell>
}
