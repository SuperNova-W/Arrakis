# ML Trade Signal Mentorship Context

> **Direction changed on 2026-07-21:** `AGENTS.md` is now the authoritative project specification.
> The target system is an end-to-end C++20 market-signal and backtesting platform using Kafka,
> Protobuf, Eigen, XGBoost, PostgreSQL, Docker, AWS, Prometheus, and Grafana. The Python notebook and
> the historical notes below are retained only as legacy research context. Do not continue the
> implementation in Python.

## Active Build Checkpoint

Task 1, adopted on 2026-07-21, is to build and deploy the frontend before starting the C++ backend.
The frontend lives in `frontend/` and uses React, TypeScript, Vite, Recharts, and Lucide. It contains
four mock-backed views: Command Center, Signal Monitor, Backtest Lab, and Data Pipeline. Mock values
must remain visibly labeled until real C++/Kafka APIs replace them. The user will deploy the
frontend to Vercel; local work should only keep it Vercel-ready and open a development server for
interactive testing.

This file is the running handoff document for future AI agents working on this project.
Keep it updated whenever the learning plan, implementation state, or project assumptions change.

## Project Purpose

The user is a Computer Science + Applied Mathematics student focused on Machine Learning.
They have completed an introductory ML course covering supervised learning, unsupervised learning,
reinforcement learning, deep learning, feature engineering, model selection, XGBoost, PCA, and the
Scikit-Learn workflow.

They also have professional software engineering experience building ML pipelines, recommendation
systems, and deploying models on AWS.

The long-term project is a machine-learning-based stock market signal. The objective is educational
and portfolio-oriented, not rapid profitability or automated live trading.

Active direction from 2026-07-09: the user does not want to spend an abnormal amount of time on
standalone learning before building. The project should now use a build-first mentorship style:
learn concepts while implementing the real research pipeline and real models.

The project may include two connected tracks:

1. A market-data signal pipeline using prices, returns, technical/rolling features, ML baselines,
   walk-forward validation, and realistic backtesting.
2. An NLP sentiment addon using FinBERT and an LSTM-style sequence model after the market-only
   baseline is reproducible.

## Mentorship Style

Teach like a research mentor pairing with the user on a real implementation.

Pacing update from 2026-07-03: the user requested a significantly faster pace and clarified that
they are a CS + Applied Math sophomore who has already taken intro AI/ML. Use a more advanced,
implementation-first style. Skip elementary pandas/ML explanations unless needed, combine lessons
into research sprints, and focus on targets, leakage, validation, baselines, backtesting, and
financial interpretation.

Pacing update from 2026-07-09: stop treating the project as a long prerequisite lesson sequence.
Default to building the real model now. Explain the theory only when it directly supports the
current implementation decision.

Style update from 2026-07-09: the user asked for much more inline code commentary because the pace
was hard to follow. Future notebook/code additions should be heavily commented, especially around
target alignment, leakage prevention, walk-forward validation, backtesting assumptions, and why each
feature exists.

Default work loop:

1. Pick the next concrete model/pipeline milestone.
2. Implement or pair-program the smallest useful slice in the actual repo.
3. Explain the intuition, math, and common mistakes only as needed for that slice.
4. Review results for leakage, validation quality, and financial realism.
5. Decide the next experiment from evidence, not from a fixed curriculum.

Do not require standalone toy lessons or review checkpoints unless the user asks for them. Keep
explanations concise, advanced, and tied to the current code.

## Constraints And Preferences

- Prioritize building the real research model now while learning in context.
- Prefer real notebook/script changes over isolated toy exercises.
- Keep iteration fast: baseline, validate, diagnose, improve.
- Do not build a trading bot.
- Do not imply that the project is likely to become profitable.
- Use examples and intuition before equations when explaining new material, but keep it concise.
- Keep implementation steps small, reviewable, and connected to the final project.
- Add generous inline comments in notebooks so the user can learn while reading the real code.
- Emphasize time-series validation, leakage prevention, realistic backtesting, and financial metrics.
- Treat this as a research-and-portfolio build, not a slow course.

## Target Final Project

The eventual project should become a clean, reproducible research pipeline that can:

1. Download historical market data.
2. Engineer predictive features.
3. Define prediction targets such as future returns or direction.
4. Train baseline models such as Logistic Regression, Random Forest, and XGBoost.
5. Use proper time-series validation, especially walk-forward evaluation.
6. Backtest generated signals.
7. Evaluate with financial metrics, not only ML metrics.
8. Account for transaction costs, bid-ask spread, and slippage.
9. Compare against benchmarks such as buy-and-hold, moving averages, and momentum.
10. Optionally ingest timestamped financial text/news data for an NLP sentiment addon.
11. Add FinBERT sentiment/embedding features and a FinBERT-LSTM sequence model as a second-stage
    research extension.
12. Present the work as a clean GitHub portfolio project.

## Concise Build Roadmap

The project should proceed as real build sprints:

1. Market-data foundation: clean OHLCV data, adjusted prices, returns, rolling windows, lagged
   features, and leakage-safe target creation.
2. Benchmarks: buy-and-hold, 21-day momentum, moving-average crossover, and volatility-filter
   strategies.
3. Tabular ML baselines: Logistic Regression or Elastic Net, Random Forest/ExtraTrees, and XGBoost.
4. Target experiments: next-day direction, thresholded returns, multi-day horizons, class imbalance,
   and probability calibration.
5. Validation and diagnostics: expanding walk-forward validation, fold-by-fold metrics, feature
   importance/stability, regime splits, and error analysis.
6. Backtesting layer: convert probabilities to positions, include transaction costs/slippage, compare
   Sharpe, Sortino, CAGR, max drawdown, turnover, hit rate, and profit factor.
7. Optional time-series deep models: LSTM/GRU/TCN only after tabular baselines are stable enough to
   justify the extra complexity.
8. NLP addon: collect timestamped financial news/headlines, run FinBERT sentiment or embeddings,
   aggregate text signals by trading day, then train a FinBERT-LSTM sequence model.
9. Model fusion: combine market features and NLP features with late fusion, stacking, or a combined
   feature table; compare against the market-only baseline.
10. Portfolio finish: organize code, freeze experiments, document methodology, limitations, results,
    and next steps.

## Current Project State

Current checkpoint from 2026-07-09: the user asked to remove the old notebook work and start from
the beginning. The active notebook has been reset to a fresh IWM build.

Current active file:

- `Short-Term directional-Signal.ipynb`
- Target asset: `IWM`, the iShares Russell 2000 ETF.
- Current notebook state: first IWM market-only baseline. It downloads IWM data, normalizes OHLCV
  columns, computes daily returns, creates `future_ret_1d`, creates `target_up_1d`, engineers the
  first leakage-safe feature table, runs a walk-forward Logistic Regression baseline, and compares
  it against buy-and-hold and 21-day momentum.
- Starter data sanity numbers from 2026-07-09:
  - Rows after dropping target/return nulls: 4,151.
  - Date range: 2010-01-05 to 2026-07-08.
  - Average daily return: about 0.000519.
  - Daily volatility: about 0.014006.
  - Next-day up rate: about 0.5363.
- First baseline executed successfully on 2026-07-09:
  - Feature rows: 3,953.
  - Feature date range: 2010-10-18 to 2026-07-08.
  - Feature count: 18.
  - Walk-forward folds: 43.
  - OOS range: 2015-10-20 to 2026-07-08.
  - Logistic Regression accuracy: about 0.498.
  - Balanced accuracy: about 0.499.
  - ROC AUC: about 0.487.
  - Buy-and-hold Sharpe: about 0.560.
  - 21-day momentum Sharpe: about 0.485.
  - Best logistic threshold rule Sharpe: about 0.208 at `proba_up > 0.50`.
  - Result interpretation: the first logistic baseline is weaker than benchmarks and probably does
    not have usable next-day directional edge as currently specified.
- Diagnostic and 5-day target experiment added and executed successfully on 2026-07-09:
  - One-day fold diagnostics:
    - Mean fold ROC AUC: about 0.500.
    - Fold ROC AUC standard deviation: about 0.083.
    - Folds above 0.50 AUC: 20 of 43.
    - Interpretation: the weak one-day result is not just one bad fold; skill is unstable.
  - 5-day target experiment:
    - Horizon: 5 trading days.
    - Feature rows: 3,949.
    - Date range: 2010-10-18 to 2026-07-01.
    - 5-day up rate: about 0.557.
    - Walk-forward folds: 43.
    - OOS range: 2015-10-20 to 2026-07-01.
    - Accuracy: about 0.498.
    - Balanced accuracy: about 0.499.
    - ROC AUC: about 0.489.
    - Non-overlapping 5-day buy-and-hold Sharpe: about 0.553.
    - Non-overlapping 5-day 21-day momentum Sharpe: about 0.395.
    - Best 5-day logistic threshold rule Sharpe: about 0.362 at `proba_up > 0.50`.
    - Interpretation: simply lengthening the target horizon did not create a useful edge.

Completed background:

- User created a fake daily price series.
- User correctly computed simple returns with `.pct_change()`.
- User correctly computed log returns with `np.log(prices / prices.shift(1))`.
- User printed prices, simple returns, and log returns side by side.
- User's written explanation correctly identified that raw dollar gains can be misleading because
  they ignore the amount of capital invested.

Volatility and risk concepts can be taught inline as they appear in features/backtests:

- Volatility measures how much returns fluctuate.
- Risk is not only whether returns are positive or negative, but how unstable they are.
- Daily volatility can be estimated with `returns.std()`.
- Rolling volatility can be estimated with `returns.rolling(window).std()`.
- Annualized volatility is commonly approximated as `daily_volatility * sqrt(252)` for daily market
  data.

Prediction-target concepts to keep emphasizing during implementation:

- A model needs a target `y`, not just input features `X`.
- For short-term directional prediction, a common target is whether the next return is positive.
- Future returns must be shifted backward into the current row with care.
- Features must only use information available at or before the prediction time.
- The most important early mistake to prevent is lookahead leakage.

Implementation note:

- In the user's environment, `yfinance.download(...)` can return MultiIndex columns with levels like
  `("Close", ticker)`. Flatten/select with
  `raw = raw.xs(ticker, axis=1, level="Ticker")` before renaming columns.
- If `dropna(subset=feature_cols + ...)` raises `KeyError`, it usually means the feature creation
  lines did not run successfully before `model_df` was created, or the source data columns were not
  shaped as expected.

Immediate next build sprint:

1. Add regime/context features instead of adding bigger models:
   market trend, volatility regime, rate-sensitive proxies, SPY/QQQ relative strength, and
   IWM-vs-SPY spread features.
2. Re-run the one-day and 5-day logistic baselines with the context features.
3. If context helps, inspect coefficient/fold stability and then consider stronger models.
4. If context does not help, try thresholded or volatility-adjusted targets.
5. Keep FinBERT/NLP as a later addon after the market-only IWM baseline is reproducible.

## Suggested Project Structure

Current workspace:

- `Short-Term directional-Signal.ipynb`: first IWM market-only baseline notebook.
- `AGENT_CONTEXT.md`: this handoff file.

Reasonable next project structure when the notebook starts getting too large:

- `src/data.py` for market/NLP data loading.
- `src/features.py` for price, technical, rolling, regime, and sentiment features.
- `src/targets.py` for leakage-safe target definitions.
- `src/models.py` for baseline and advanced model training.
- `src/backtest.py` for position generation and financial metrics.
- `src/validation.py` for walk-forward splitting and fold diagnostics.
- `notebooks/` for research notebooks and final result narratives.

## Future Agent Instructions

When resuming:

1. Read this file first.
2. Inspect the workspace for the user's latest implementation.
3. Continue with build-first implementation unless the user explicitly asks for a standalone lesson.
4. Review the user's work before introducing new material.
5. Update the current project state after each meaningful milestone.
6. Keep this document factual and concise.
