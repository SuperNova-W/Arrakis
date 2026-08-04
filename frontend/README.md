# Arrakis ETF Market Viewer

React and TypeScript ETF research dashboard. Historical chart candles come from Twelve Data REST;
quotes and fund metadata come from Finnhub REST. The Arrakis `market-api` WebSocket is connected
separately, purely as a connectivity indicator for the backend inference pipeline — it is not merged
into the chart. The frontend does not call database-backed bar REST endpoints or Supabase.

## Market views

- Live quote cards for the configured sector, broad-market, factor, and macro ETFs (Finnhub).
- Interactive area and candlestick charts with volume and a precise crosshair readout (Twelve Data).
- 1D, 5D, 1M, 3M, 6M, YTD, 1Y, 5Y, and MAX ranges.
- SMA, EMA, Bollinger Bands, RSI, and MACD indicators calculated in a Web Worker.
- Optional benchmark comparison and regular-session/extended-hours filtering.
- Range return, annualized volatility, drawdown, high/low, average volume, and CSV export.
- Finnhub ETF profile, holdings, and sector exposure when the API subscription includes them.
- An "Inference feed" status badge on the ETF detail page showing the Arrakis `market-api`
  WebSocket's connection state (connecting/live/reconnecting/offline). This reflects the backend
  inference pipeline's data path only — it never changes what the chart displays.
- IndexedDB response caching, request deduplication, bounded concurrency, and stale-cache recovery
  (both Twelve Data and Finnhub requests).

ML recommendations remain visually separate from market data and are explicitly unavailable until the
independent backend inference pipeline is connected — the Recommendations page states this directly
rather than showing fabricated output. The application is research-only and never places trades.

## Data provider requirements

Two independent, browser-direct REST integrations power the dashboard:

- **Twelve Data** (`VITE_TWELVE_DATA_API_KEY`) — historical OHLCV candles for every chart range.
  Required for the chart to render at all; there is no fallback provider.
- **Finnhub** (`VITE_FINNHUB_API_KEY`, or the in-app session-key setup screen) — live quotes, ETF
  profile, holdings, and sector exposure. The application accepts a Finnhub key on its setup screen
  and keeps it in `sessionStorage`, cleared when the browser session ends. A key that only includes
  real-time quotes will still populate the dashboard, while the chart-adjacent composition panels
  show a precise entitlement message if the plan doesn't include `etf/profile`.

For local development, copy `.env.example` to `.env` and set both keys directly — Vite embeds them
into the bundle at build time, which is convenient for local/single-user use but means the keys are
visible to anyone with the built JS. Do not embed a shared production secret in a deployed `VITE_`
environment variable without accepting that tradeoff.

`VITE_MARKET_API_WS_URL` configures the Arrakis `market-api` WebSocket used only for the inference
feed status badge. It defaults to `ws://<current-host>:8080/ws/v1/market` if unset, which only works
when a `market-api` instance is reachable on that host/port (true for local Docker Compose; not true
for a plain Vercel deployment unless you point it at a publicly reachable backend).

## Development

```bash
npm install
npm run dev
```

## Verification

```bash
npm run lint
npm run build
npm run preview
```

## Deployment

The app is configured for Vercel as a Vite project. Import the repository with `frontend/` as the
root directory; `vercel.json` supplies the build command and output directory. Set
`VITE_FINNHUB_API_KEY` and `VITE_TWELVE_DATA_API_KEY` as environment variables in the Vercel project
settings before deploying — they are not committed anywhere in the repo, so the build will have no
chart or quote data without them. `VITE_MARKET_API_WS_URL` is optional; without a publicly reachable
`market-api` backend, the inference feed badge will simply show as offline, which is expected.
