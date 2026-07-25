# Arrakis

Arrakis is a real-time market signal and backtesting platform under active development. The current
repository contains a React product prototype, a C++20 XGBoost training baseline, and a C++20
Boost.Beast client for Finnhub's real-time stock-trade WebSocket feed.

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
See [`backend/README.md`](backend/README.md) for the C++ model trainer, live market stream, data
contracts, and build instructions.

> All market, strategy, model, and infrastructure values currently shown in the application are
> simulated mock data. They are not evidence of live services or validated trading performance.
