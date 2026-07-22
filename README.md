# Arrakis

Arrakis is the frontend for a real-time market signal and backtesting platform. The current release
is a React and TypeScript product prototype backed by clearly labeled mock data.

## Current interface

- Command Center for portfolio, model, and signal summaries
- Signal Monitor for confidence, feature influence, and prediction events
- Backtest Lab for simulated performance and walk-forward diagnostics
- Data Pipeline view for Kafka topology, service health, lag, and throughput

## Run locally

```bash
cd frontend
npm install
npm run dev
```

## Verify

```bash
cd frontend
npm run lint
npm run build
```

See [`frontend/README.md`](frontend/README.md) for frontend details and Vercel deployment settings.

> All market, strategy, model, and infrastructure values currently shown in the application are
> simulated mock data. They are not evidence of live services or validated trading performance.
