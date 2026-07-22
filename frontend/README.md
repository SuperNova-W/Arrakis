# Arrakis Market Intelligence Dashboard

Task 1 of the Real-Time Market Signal and Backtesting Platform: a React and TypeScript frontend
that presents realistic mock data for the future C++20/Kafka system.

## Included views

- **Command center:** portfolio simulation, model status, equity curve, and recent signals.
- **Signal monitor:** prediction confidence, feature influence, calibration, and event filtering.
- **Backtest lab:** simulated performance, walk-forward fold stability, and execution assumptions.
- **Data pipeline:** Kafka topology, service health, consumer lag, topic rate, and retention.

Every financial and operational value is explicitly mock data. The interface does not claim a live
backend, deployed C++ service, or validated trading performance.

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

The app is configured for Vercel as a Vite project. Import the repository in Vercel with
`frontend/` as the root directory; `vercel.json` supplies the build command and output directory.

```bash
npm run build
npx vercel
```
