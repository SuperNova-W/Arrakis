# Arrakis ETF Market Viewer

React and TypeScript ETF research dashboard. Historical candles, quotes, and fund metadata come
from Finnhub REST; the chart’s current 1m/5m edge comes from the Arrakis market-api WebSocket.
The frontend does not call database-backed bar REST endpoints or Supabase.

## Market views

- Live quote cards for the configured sector, broad-market, factor, and macro ETFs.
- Interactive area and candlestick charts with volume and a precise crosshair readout.
- 1D, 5D, 1M, 3M, 6M, YTD, 1Y, 5Y, and MAX ranges.
- SMA, EMA, Bollinger Bands, RSI, and MACD indicators calculated in a Web Worker.
- Optional benchmark comparison and regular-session/extended-hours filtering.
- Range return, annualized volatility, drawdown, high/low, average volume, and CSV export.
- Finnhub ETF profile, holdings, and sector exposure when the API subscription includes them.
- Shared market-api WebSocket connection with reconnecting/offline status and live candle merging.
- IndexedDB response caching, request deduplication, bounded concurrency, and stale-cache recovery.

ML recommendations remain visually separate from market data and are explicitly unavailable without
the independent inference service. The application is research-only and never places trades.

## Finnhub requirements

The application accepts a Finnhub API key on its setup screen and keeps it in `sessionStorage`, so it
is cleared when the browser session ends. For local development you can alternatively copy
`.env.example` to `.env.local` and set `VITE_FINNHUB_API_KEY`. `VITE_MARKET_API_WS_URL` defaults to
`ws://<current-host>:8080/ws/v1/market` for local Docker Compose use, and can be overridden for a
different backend host.

The ETF charts require access to Finnhub's `stock/candle` endpoint. Fund composition requires
`etf/profile`. A key that only includes real-time quotes will still populate the dashboard, while the
chart and composition panels show a precise entitlement message.

Because this is a browser-direct Finnhub integration, the key is visible to the person using the browser.
Do not embed a shared production secret in a deployed `VITE_` environment variable.

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

The app is configured for Vercel as a Vite project. Import the repository with `frontend/` as the
root directory; `vercel.json` supplies the build command and output directory.
