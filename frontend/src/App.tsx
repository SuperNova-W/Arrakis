import { useMemo, useState } from 'react'
import {
  Activity,
  ArrowDownRight,
  ArrowRight,
  ArrowUpRight,
  BarChart3,
  Bell,
  Boxes,
  BrainCircuit,
  ChevronDown,
  CircleDot,
  Clock3,
  Database,
  Gauge,
  GitBranch,
  Layers3,
  Menu,
  Pause,
  Play,
  Radio,
  Search,
  ServerCog,
  ShieldCheck,
  SlidersHorizontal,
  X,
} from 'lucide-react'
import {
  Area,
  AreaChart,
  Bar,
  BarChart,
  CartesianGrid,
  Cell,
  Line,
  ReferenceLine,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts'
import {
  equityCurve,
  featureContributions,
  foldPerformance,
  recentSignals,
  services,
  topicMetrics,
  type SignalDirection,
} from './data/mockData'

type PageId = 'overview' | 'signals' | 'backtests' | 'pipeline'
type TimeRange = '1M' | '3M' | '6M'

const rangeLengths: Record<TimeRange, number> = { '1M': 21, '3M': 63, '6M': 126 }

const pages: Array<{ id: PageId; label: string; icon: typeof Gauge }> = [
  { id: 'overview', label: 'Command center', icon: Gauge },
  { id: 'signals', label: 'Signal monitor', icon: Radio },
  { id: 'backtests', label: 'Backtest lab', icon: BarChart3 },
  { id: 'pipeline', label: 'Data pipeline', icon: GitBranch },
]

const pageMeta: Record<PageId, { eyebrow: string; title: string; description: string }> = {
  overview: {
    eyebrow: 'Operations / Overview',
    title: 'Command center',
    description: 'A unified view of live signals, model behavior, and portfolio simulation.',
  },
  signals: {
    eyebrow: 'Intelligence / Signals',
    title: 'Signal monitor',
    description: 'Inspect prediction confidence, feature influence, and the latest model events.',
  },
  backtests: {
    eyebrow: 'Research / Evaluation',
    title: 'Backtest lab',
    description: 'Interrogate out-of-sample performance across time, folds, and market regimes.',
  },
  pipeline: {
    eyebrow: 'Infrastructure / Streaming',
    title: 'Data pipeline',
    description: 'Monitor event flow, consumer health, latency, and Kafka topic activity.',
  },
}

function formatMoney(value: number) {
  return new Intl.NumberFormat('en-US', {
    style: 'currency',
    currency: 'USD',
    minimumFractionDigits: 2,
  }).format(value)
}

function DirectionBadge({ direction }: { direction: SignalDirection }) {
  const icon =
    direction === 'LONG' ? <ArrowUpRight size={13} /> :
      direction === 'SHORT' ? <ArrowDownRight size={13} /> : <ArrowRight size={13} />

  return <span className={`direction-badge direction-${direction.toLowerCase()}`}>{icon}{direction}</span>
}

function MetricCard({
  label,
  value,
  delta,
  detail,
  tone = 'positive',
}: {
  label: string
  value: string
  delta: string
  detail: string
  tone?: 'positive' | 'negative' | 'neutral'
}) {
  return (
    <article className="metric-card">
      <div className="metric-card-top">
        <span>{label}</span>
        <span className={`metric-delta ${tone}`}>{delta}</span>
      </div>
      <strong>{value}</strong>
      <p>{detail}</p>
    </article>
  )
}

function ChartTooltip({ active, payload, label }: {
  active?: boolean
  payload?: Array<{ name: string; value: number; color: string }>
  label?: string
}) {
  if (!active || !payload?.length) return null

  return (
    <div className="chart-tooltip">
      <span>{label}</span>
      {payload.map((item) => (
        <div key={item.name}>
          <i style={{ background: item.color }} />
          <span>{item.name}</span>
          <strong>{item.value.toFixed(2)}</strong>
        </div>
      ))}
    </div>
  )
}

function EquityChart({ compact = false }: { compact?: boolean }) {
  const [range, setRange] = useState<TimeRange>('6M')
  const data = useMemo(() => equityCurve.slice(-rangeLengths[range]), [range])

  return (
    <section className={`panel chart-panel ${compact ? 'chart-panel-compact' : ''}`}>
      <div className="panel-heading">
        <div>
          <span className="section-kicker">SIMULATED PERFORMANCE</span>
          <h2>Strategy equity</h2>
        </div>
        <div className="range-switcher" aria-label="Chart time range">
          {(['1M', '3M', '6M'] as TimeRange[]).map((item) => (
            <button
              className={range === item ? 'active' : ''}
              key={item}
              onClick={() => setRange(item)}
            >
              {item}
            </button>
          ))}
        </div>
      </div>
      <div className="chart-legend">
        <span><i className="legend-strategy" />Arrakis signal <strong>+22.48%</strong></span>
        <span><i className="legend-benchmark" />Buy &amp; hold <strong>+11.72%</strong></span>
      </div>
      <div className="equity-chart" aria-label="Simulated strategy versus benchmark equity chart">
        <ResponsiveContainer width="100%" height="100%">
          <AreaChart data={data} margin={{ top: 12, right: 4, left: -24, bottom: 0 }}>
            <defs>
              <linearGradient id="strategyFill" x1="0" y1="0" x2="0" y2="1">
                <stop offset="0%" stopColor="#7f9fbd" stopOpacity={0.16} />
                <stop offset="100%" stopColor="#7f9fbd" stopOpacity={0} />
              </linearGradient>
            </defs>
            <CartesianGrid stroke="#202832" strokeDasharray="3 5" vertical={false} />
            <XAxis dataKey="date" axisLine={false} tickLine={false} tick={{ fill: '#687482', fontSize: 11 }} minTickGap={32} />
            <YAxis axisLine={false} tickLine={false} tick={{ fill: '#687482', fontSize: 11 }} domain={['dataMin - 3', 'dataMax + 3']} />
            <Tooltip content={<ChartTooltip />} />
            <Area type="monotone" dataKey="strategy" name="Arrakis signal" stroke="#7f9fbd" strokeWidth={2} fill="url(#strategyFill)" activeDot={{ r: 3, fill: '#7f9fbd', stroke: '#10151c' }} isAnimationActive={false} />
            <Line type="monotone" dataKey="benchmark" name="Buy & hold" stroke="#637083" strokeWidth={1.4} dot={false} strokeDasharray="5 4" isAnimationActive={false} />
          </AreaChart>
        </ResponsiveContainer>
      </div>
      <p className="mock-note">Mock research environment · costs included at 5 bps per position change</p>
    </section>
  )
}

function SignalTable({ limit }: { limit?: number }) {
  const rows = limit ? recentSignals.slice(0, limit) : recentSignals

  return (
    <div className="table-wrap">
      <table>
        <thead>
          <tr>
            <th>Time</th>
            <th>Asset</th>
            <th>Signal</th>
            <th>Confidence</th>
            <th>Regime</th>
            <th>Model</th>
          </tr>
        </thead>
        <tbody>
          {rows.map((signal) => (
            <tr key={signal.id}>
              <td className="mono muted">{signal.time}</td>
              <td>
                <div className="asset-cell">
                  <span>{signal.symbol}</span>
                  <small>{formatMoney(signal.price)}</small>
                </div>
              </td>
              <td><DirectionBadge direction={signal.direction} /></td>
              <td>
                <div className="confidence-cell">
                  <div><span style={{ width: `${signal.confidence}%` }} /></div>
                  <strong>{signal.confidence.toFixed(1)}%</strong>
                </div>
              </td>
              <td>{signal.regime}</td>
              <td className="mono muted">{signal.model}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}

function OverviewPage() {
  return (
    <>
      <div className="market-strip">
        <div className="featured-quote">
          <div className="ticker-mark">IW</div>
          <div>
            <span>IWM · Russell 2000 ETF</span>
            <strong>$227.80</strong>
          </div>
          <span className="quote-change"><ArrowUpRight size={15} /> 1.14%</span>
        </div>
        <div className="signal-callout">
          <span className="section-kicker">LATEST MODEL DECISION</span>
          <div><DirectionBadge direction="LONG" /><strong>68.4% confidence</strong></div>
          <p>Regime filter confirms broadening risk appetite.</p>
        </div>
        <div className="market-state">
          <span className="section-kicker">MARKET REGIME</span>
          <strong><Activity size={17} /> Risk-on expansion</strong>
          <p>Moderate volatility · positive breadth</p>
        </div>
      </div>

      <div className="metric-grid">
        <MetricCard label="Portfolio value" value="$122,481" delta="+1.84%" detail="+$2,214 today" />
        <MetricCard label="OOS Sharpe" value="1.42" delta="+0.18" detail="63-day rolling: 1.61" />
        <MetricCard label="Max drawdown" value="−8.73%" delta="2.4% below limit" detail="Recovered in 31 sessions" tone="neutral" />
        <MetricCard label="Model ROC AUC" value="0.582" delta="+0.031" detail="Across 43 walk-forward folds" />
      </div>

      <div className="dashboard-grid">
        <EquityChart />
        <aside className="panel model-card">
          <div className="panel-heading">
            <div>
              <span className="section-kicker">ACTIVE MODEL</span>
              <h2>XGBoost v0.8.3</h2>
            </div>
            <span className="live-chip"><CircleDot size={12} /> SERVING</span>
          </div>
          <div className="model-score">
            <div className="score-ring"><span>68</span><small>/100</small></div>
            <div><strong>Bullish conviction</strong><span>Calibrated probability</span></div>
          </div>
          <div className="model-facts">
            <div><span>Prediction horizon</span><strong>5 trading days</strong></div>
            <div><span>Last retrained</span><strong>Jul 18 · 02:00 UTC</strong></div>
            <div><span>Feature count</span><strong>34 active / 42 total</strong></div>
            <div><span>Inference p95</span><strong>3.7 ms</strong></div>
          </div>
          <button className="text-button">Inspect model card <ArrowRight size={14} /></button>
        </aside>
      </div>

      <section className="panel signals-panel">
        <div className="panel-heading">
          <div>
            <span className="section-kicker">EVENT STREAM</span>
            <h2>Recent signals</h2>
          </div>
          <button className="secondary-button">View all events <ArrowRight size={14} /></button>
        </div>
        <SignalTable limit={5} />
      </section>
    </>
  )
}

function SignalsPage() {
  const [filter, setFilter] = useState<'ALL' | SignalDirection>('ALL')
  const filtered = filter === 'ALL' ? recentSignals : recentSignals.filter((signal) => signal.direction === filter)

  return (
    <>
      <div className="signal-hero">
        <div>
          <span className="section-kicker">PRIMARY ASSET · IWM</span>
          <div className="hero-decision"><DirectionBadge direction="LONG" /><strong>68.4%</strong></div>
          <p>Model probability exceeds the 61% execution threshold by 7.4 points.</p>
        </div>
        <div className="probability-scale">
          <div className="probability-labels"><span>Short</span><span>Flat</span><span>Long</span></div>
          <div className="probability-track"><span style={{ left: '68.4%' }} /></div>
          <div className="threshold-marker" style={{ left: '61%' }}>threshold</div>
        </div>
      </div>

      <div className="signals-layout">
        <section className="panel feature-panel">
          <div className="panel-heading">
            <div><span className="section-kicker">LOCAL EXPLANATION</span><h2>Feature influence</h2></div>
            <button className="icon-button" aria-label="Feature settings"><SlidersHorizontal size={16} /></button>
          </div>
          <p className="panel-description">Contribution to the latest calibrated IWM prediction.</p>
          <div className="feature-list">
            {featureContributions.map((feature) => (
              <div className="feature-row" key={feature.name}>
                <div><strong>{feature.name}</strong><span>value {feature.value.toFixed(2)}</span></div>
                <div className="feature-bar"><span className={feature.direction} style={{ width: `${feature.contribution * 4}%` }} /></div>
                <strong className={feature.direction}>{feature.direction === 'positive' ? '+' : '−'}{feature.contribution.toFixed(1)}%</strong>
              </div>
            ))}
          </div>
        </section>

        <section className="panel calibration-card">
          <div className="panel-heading"><div><span className="section-kicker">CALIBRATION</span><h2>Confidence quality</h2></div></div>
          <div className="calibration-score"><strong>0.041</strong><span>Brier score</span></div>
          <div className="calibration-grid">
            <div><span>Expected calibration error</span><strong>2.8%</strong></div>
            <div><span>Samples (90d)</span><strong>22,680</strong></div>
            <div><span>Threshold</span><strong>61.0%</strong></div>
          </div>
          <div className="calibration-note"><ShieldCheck size={17} /><p>Probability calibration is within the configured 5% tolerance.</p></div>
        </section>
      </div>

      <section className="panel signals-panel">
        <div className="panel-heading filter-heading">
          <div><span className="section-kicker">LIVE EVENT LOG</span><h2>Prediction stream</h2></div>
          <div className="filter-pills">
            {(['ALL', 'LONG', 'FLAT', 'SHORT'] as const).map((item) => (
              <button className={filter === item ? 'active' : ''} key={item} onClick={() => setFilter(item)}>{item}</button>
            ))}
          </div>
        </div>
        <div className="table-wrap">
          <table>
            <thead><tr><th>Time</th><th>Asset</th><th>Signal</th><th>Confidence</th><th>Regime</th><th>Model</th></tr></thead>
            <tbody>
              {filtered.map((signal) => (
                <tr key={signal.id}>
                  <td className="mono muted">{signal.time}</td>
                  <td><div className="asset-cell"><span>{signal.symbol}</span><small>{formatMoney(signal.price)}</small></div></td>
                  <td><DirectionBadge direction={signal.direction} /></td>
                  <td><div className="confidence-cell"><div><span style={{ width: `${signal.confidence}%` }} /></div><strong>{signal.confidence.toFixed(1)}%</strong></div></td>
                  <td>{signal.regime}</td><td className="mono muted">{signal.model}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </section>
    </>
  )
}

function BacktestsPage() {
  return (
    <>
      <div className="research-banner">
        <div><BrainCircuit size={21} /><span>RESEARCH RUN</span><strong>iwm_xgb_5d_costs_v083</strong></div>
        <div className="research-meta"><span>2015-10-20 → 2026-07-01</span><span>43 walk-forward folds</span><span>5 bps costs</span></div>
        <button className="secondary-button">Compare runs <ChevronDown size={14} /></button>
      </div>

      <div className="metric-grid backtest-metrics">
        <MetricCard label="Net CAGR" value="15.81%" delta="+5.72%" detail="vs. buy & hold" />
        <MetricCard label="Sortino ratio" value="2.08" delta="+0.43" detail="Downside deviation: 7.6%" />
        <MetricCard label="Profit factor" value="1.37" delta="+0.12" detail="2,106 closed positions" />
        <MetricCard label="Annual turnover" value="3.8×" delta="−0.4×" detail="After threshold optimization" tone="neutral" />
      </div>

      <EquityChart compact />

      <div className="backtest-grid">
        <section className="panel fold-panel">
          <div className="panel-heading"><div><span className="section-kicker">STABILITY CHECK</span><h2>Fold-by-fold ROC AUC</h2></div><span className="panel-tag">Mean 0.582</span></div>
          <div className="fold-chart">
            <ResponsiveContainer width="100%" height="100%">
              <BarChart data={foldPerformance} margin={{ top: 14, right: 4, left: -28, bottom: 0 }}>
                <CartesianGrid stroke="#202832" strokeDasharray="3 5" vertical={false} />
                <XAxis dataKey="fold" axisLine={false} tickLine={false} tick={{ fill: '#687482', fontSize: 10 }} interval={2} />
                <YAxis domain={[0.4, 0.65]} axisLine={false} tickLine={false} tick={{ fill: '#687482', fontSize: 10 }} />
                <ReferenceLine y={0.5} stroke="#758092" strokeDasharray="4 4" />
                <Tooltip content={<ChartTooltip />} />
                <Bar dataKey="auc" name="ROC AUC" radius={[2, 2, 0, 0]} isAnimationActive={false}>
                  {foldPerformance.map((entry) => <Cell key={entry.fold} fill={entry.auc >= 0.5 ? '#71947a' : '#b97575'} fillOpacity={0.8} />)}
                </Bar>
              </BarChart>
            </ResponsiveContainer>
          </div>
        </section>

        <section className="panel assumptions-panel">
          <div className="panel-heading"><div><span className="section-kicker">EXECUTION MODEL</span><h2>Backtest assumptions</h2></div></div>
          <div className="assumption-list">
            <div><span>Transaction cost</span><strong>5 bps / change</strong></div>
            <div><span>Bid–ask spread</span><strong>3 bps round trip</strong></div>
            <div><span>Slippage model</span><strong>Volatility-adjusted</strong></div>
            <div><span>Execution delay</span><strong>1 bar</strong></div>
            <div><span>Maximum exposure</span><strong>100% long / cash</strong></div>
            <div><span>Probability threshold</span><strong>61%</strong></div>
          </div>
          <div className="warning-note"><Activity size={17} /><p>Mock results for product development only. They do not represent a validated trading strategy.</p></div>
        </section>
      </div>
    </>
  )
}

function PipelinePage() {
  return (
    <>
      <div className="pipeline-summary">
        <div><span className="pulse-dot" /><div><span>CLUSTER STATUS</span><strong>5 healthy · 1 degraded</strong></div></div>
        <div><span>Aggregate throughput</span><strong>8,247 msg/s</strong></div>
        <div><span>End-to-end p95</span><strong>64.8 ms</strong></div>
        <div><span>Total consumer lag</span><strong>201 events</strong></div>
      </div>

      <section className="panel topology-panel">
        <div className="panel-heading"><div><span className="section-kicker">LIVE TOPOLOGY</span><h2>Event processing path</h2></div><span className="live-chip"><CircleDot size={12} /> STREAMING</span></div>
        <div className="topology-canvas">
          {[
            { icon: Radio, label: 'Market ingest', topic: 'market.raw.v1' },
            { icon: ShieldCheck, label: 'Validator', topic: 'market.validated.v1' },
            { icon: Layers3, label: 'Feature engine', topic: 'features.realtime.v1' },
            { icon: BrainCircuit, label: 'Inference', topic: 'signals.model.v1' },
            { icon: Database, label: 'Signal store', topic: 'PostgreSQL' },
          ].map((node, index, all) => {
            const Icon = node.icon
            return (
              <div className="topology-segment" key={node.label}>
                <div className="topology-node"><span><Icon size={20} /></span><strong>{node.label}</strong><small>{node.topic}</small></div>
                {index < all.length - 1 && <div className="topology-link"><i /><i /><i /><ArrowRight size={17} /></div>}
              </div>
            )
          })}
        </div>
      </section>

      <section className="panel services-panel">
        <div className="panel-heading"><div><span className="section-kicker">CONSUMER GROUPS</span><h2>Service health</h2></div><button className="secondary-button"><ServerCog size={14} /> Manage services</button></div>
        <div className="service-grid">
          {services.map((service) => (
            <article className="service-card" key={service.name}>
              <div className="service-card-title"><span className={`status-light ${service.state.toLowerCase()}`} /><div><strong>{service.name}</strong><span>{service.description}</span></div><em className={service.state.toLowerCase()}>{service.state}</em></div>
              <div className="service-stats"><div><span>Throughput</span><strong>{service.throughput}</strong></div><div><span>p95 latency</span><strong>{service.latency}</strong></div><div><span>Lag</span><strong>{service.consumerLag}</strong></div></div>
            </article>
          ))}
        </div>
      </section>

      <section className="panel topics-panel">
        <div className="panel-heading"><div><span className="section-kicker">KAFKA</span><h2>Topic activity</h2></div></div>
        <div className="table-wrap"><table><thead><tr><th>Topic</th><th>Partitions</th><th>Message rate</th><th>Retention</th><th>Storage</th></tr></thead><tbody>{topicMetrics.map((topic) => <tr key={topic.topic}><td className="topic-name"><Boxes size={14} />{topic.topic}</td><td>{topic.partitions}</td><td className="positive-text">{topic.rate}</td><td>{topic.retention}</td><td>{topic.size}</td></tr>)}</tbody></table></div>
      </section>
    </>
  )
}

function App() {
  const [activePage, setActivePage] = useState<PageId>('overview')
  const [isReplayLive, setIsReplayLive] = useState(true)
  const [mobileNavOpen, setMobileNavOpen] = useState(false)
  const meta = pageMeta[activePage]

  const selectPage = (page: PageId) => {
    setActivePage(page)
    setMobileNavOpen(false)
    window.scrollTo({ top: 0, behavior: 'smooth' })
  }

  return (
    <div className="app-shell">
      <aside className={`sidebar ${mobileNavOpen ? 'mobile-open' : ''}`}>
        <div className="brand"><div className="brand-mark"><BarChart3 size={18} /></div><div><strong>ARRAKIS</strong><span>MARKET INTELLIGENCE</span></div></div>
        <button className="close-nav" aria-label="Close navigation" onClick={() => setMobileNavOpen(false)}><X size={20} /></button>
        <nav aria-label="Primary navigation">
          <span className="nav-label">WORKSPACE</span>
          {pages.map((page) => {
            const Icon = page.icon
            return <button key={page.id} className={activePage === page.id ? 'active' : ''} onClick={() => selectPage(page.id)}><Icon size={17} /><span>{page.label}</span>{activePage === page.id && <i />}</button>
          })}
        </nav>
        <div className="sidebar-bottom">
          <div className="environment-card"><div><span className="pulse-dot" /><strong>Mock environment</strong></div><p>All values are simulated</p></div>
          <div className="sidebar-user"><span>SW</span><div><strong>Research workspace</strong><small>Local simulation</small></div><ChevronDown size={14} /></div>
        </div>
      </aside>

      {mobileNavOpen && <button className="nav-scrim" aria-label="Close navigation" onClick={() => setMobileNavOpen(false)} />}

      <main>
        <header className="topbar">
          <button className="mobile-menu" aria-label="Open navigation" onClick={() => setMobileNavOpen(true)}><Menu size={20} /></button>
          <div className="breadcrumbs"><span>ARRAKIS</span><ArrowRight size={12} /><strong>{meta.title}</strong></div>
          <div className="topbar-actions">
            <label className="search-box"><Search size={15} /><input aria-label="Search dashboard" placeholder="Search assets, runs, events" /></label>
            <button className="icon-button notification-button" aria-label="Notifications"><Bell size={17} /><i /></button>
            <button className={`replay-button ${isReplayLive ? 'live' : ''}`} onClick={() => setIsReplayLive((current) => !current)}>{isReplayLive ? <Pause size={14} /> : <Play size={14} />}<span>{isReplayLive ? 'Live replay' : 'Replay paused'}</span></button>
          </div>
        </header>

        <div className="page-content">
          <div className="page-title">
            <div><span>{meta.eyebrow}</span><h1>{meta.title}</h1><p>{meta.description}</p></div>
            <div className="market-clock"><Clock3 size={15} /><div><span>MARKET</span><strong>OPEN · 01:31:18</strong></div></div>
          </div>

          {activePage === 'overview' && <OverviewPage />}
          {activePage === 'signals' && <SignalsPage />}
          {activePage === 'backtests' && <BacktestsPage />}
          {activePage === 'pipeline' && <PipelinePage />}
        </div>
      </main>
    </div>
  )
}

export default App
