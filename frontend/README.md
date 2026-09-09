# Arrakis ETF research dashboard

React, TypeScript and Vite frontend for ETF prices and published research. Quotes come from
Finnhub; historical charts come from Twelve Data. Recommendations read the latest per-fund
research document from the public Supabase view. No always-on inference WebSocket is required.

The interface shows research dates and update times, distinguishes partial-day updates, and explains
forecast availability in plain language. Models that have not passed validation produce no forecast.
XLK has news coverage; other funds currently have market-only research. Article links cite sources.

Keyboard users can skip navigation, inspect selected chart controls, and open a recent-prices table
as an alternative to the chart. Prices can also be downloaded as CSV. Research never executes trades.

## Local development

Copy `.env.example` to `.env`, configure the providers, then run:

```sh
npm ci
npm run dev
```

- `VITE_FINNHUB_API_KEY`: quotes and fund metadata.
- `VITE_TWELVE_DATA_API_KEY`: historical prices.
- `VITE_SUPABASE_URL` and `VITE_SUPABASE_ANON_KEY`: read-only published research.

Vite embeds these values in browser JavaScript. The Supabase anon key is publishable; never use
a service-role key. Provider keys embedded in the frontend are visible to visitors.

## Verify and deploy

```sh
npm run lint
npm run build
```

Vercel uses `frontend/` as its root; `vercel.json` builds the app and serves client routes.
Configure the variables above for each deployed environment. The hourly workflow publishes research
independently. A successful deployment of the frontend does not imply a successful research run.
