# Real-Time Market Signal and Backtesting Platform

## Authoritative Project Goal

Build an end-to-end, production-style market-signal research and streaming platform in C++20.
Apache Kafka must connect market-data ingestion, validation, feature engineering, model training,
inference, backtesting, persistence, and risk-management components. The finished system must be
reproducible, leakage-safe, fault-tolerant, observable, benchmarked, containerized, and deployable
to AWS.

This is an educational and portfolio project. It is not an automated live-trading system, and no
claim of profitability or predictive edge may be made without reproducible out-of-sample evidence.

## Required Technology Direction

- C++20 for the complete application and research implementation.
- Apache Kafka for event transport and replayable stream processing.
- Protocol Buffers for versioned event schemas.
- Eigen for numerical routines and the custom Logistic Regression implementation.
- XGBoost for the nonlinear model baseline and comparison.
- PostgreSQL for durable market, signal, experiment, and performance data.
- Docker for local development, integration testing, and packaging.
- AWS for the eventual deployed environment.
- Prometheus and Grafana for metrics and operational dashboards.
- CMake for builds and GoogleTest for automated tests.

Do not introduce Python into the target implementation. The existing notebook is legacy research
reference only; new ingestion, feature generation, model training, validation, backtesting,
inference, and benchmarking work must be implemented in C++20. External infrastructure and tools
such as Kafka, PostgreSQL, Docker, Grafana, and AWS are naturally not subject to the C++ constraint.

## Target System Capabilities

1. Ingest historical and streaming OHLCV market data through C++ producers.
2. Validate, normalize, serialize, and publish versioned market events.
3. Maintain incremental per-symbol state for momentum, volatility, volume, relative-strength, and
   market-regime features.
4. Generate leakage-safe targets and reproducible training datasets.
5. Train an Eigen-based Logistic Regression model with stable optimization, regularization,
   standardization, and class weighting.
6. Train and benchmark XGBoost models against Logistic Regression and non-ML strategies.
7. Perform expanding-window walk-forward evaluation over multiple market regimes.
8. Run an event-driven backtester with transaction costs, bid-ask spread, slippage, turnover,
   execution delay, exposure limits, and position limits.
9. Report classification and financial metrics, including ROC AUC, balanced accuracy, Sharpe,
   Sortino, CAGR, maximum drawdown, turnover, hit rate, and profit factor.
10. Serve versioned model inference from C++ and publish signals through Kafka.
11. Persist events, predictions, model metadata, experiments, and results in PostgreSQL.
12. Provide structured logs, health checks, Prometheus metrics, Grafana dashboards, and actionable
    failure diagnostics.
13. Package services with Docker, test them in CI, and deploy the system to AWS.
14. Measure throughput and latency with reproducible benchmarks and report p50, p95, and p99 rather
    than unsubstantiated performance claims.

## Kafka Engineering Requirements

Kafka must be a substantive part of the architecture rather than a decorative transport layer.
The completed platform should demonstrate:

- Partitioning by symbol or another documented ordering key.
- Consumer groups and independently scalable services.
- Explicit offset and acknowledgement behavior.
- Idempotent production and duplicate-safe consumption.
- Protocol Buffer schema evolution and compatibility tests.
- Retry and dead-letter topics with failure metadata.
- Historical replay and deterministic rebuilding of derived state.
- Backpressure handling and bounded in-memory queues.
- Consumer-lag, throughput, error-rate, and processing-latency metrics.
- Graceful shutdown, rebalance handling, and recovery after forced restarts.
- Integration tests that exercise duplicates, out-of-order events, malformed messages, service
  interruption, and replay.

Do not describe the system as exactly-once unless Kafka transactions or an equivalent end-to-end
design have been implemented and verified. Document actual delivery semantics precisely.

## ML and Backtesting Standards

- Features on each event may use only information available at that event's prediction time.
- Preprocessing, scaling, calibration, feature selection, and threshold selection must be fit only
  on the applicable training window.
- Random train/test splits are not acceptable for time-series performance claims.
- Use expanding or rolling walk-forward evaluation with documented fold boundaries.
- Keep model-selection data separate from the final untouched evaluation period when practical.
- Compare models with buy-and-hold, momentum, and moving-average baselines.
- Separate predictive metrics from strategy metrics.
- Apply costs and execution assumptions before presenting strategy performance.
- Report negative and inconclusive experiments honestly.
- Prefer a simple, auditable baseline before increasing model complexity.
- Preserve determinism where possible through fixed seeds, versioned inputs, and recorded configs.

The legacy IWM notebook produced 43 walk-forward folds over an out-of-sample period beginning in
2015 and found no stable directional edge. Treat that result as a reference hypothesis to reproduce
or challenge in C++, not as proof of model quality.

## Implementation Principles

- Build in small, testable vertical slices that run end to end.
- Keep domain logic independent from Kafka, database, and networking adapters.
- Use RAII, clear ownership, bounded resources, and explicit error handling.
- Avoid shared mutable state where message passing or ownership transfer is clearer.
- Add unit tests for numerical logic and integration tests for service boundaries.
- Use compiler warnings, formatting, clang-tidy, AddressSanitizer, UndefinedBehaviorSanitizer, and
  ThreadSanitizer where applicable.
- Profile before optimizing, then preserve benchmark evidence for every performance claim.
- Document architectural decisions and tradeoffs as the system evolves.
- Do not build brokerage execution or place real trades unless the user explicitly changes scope.

## Suggested Build Order

1. Establish the C++20/CMake repository, dependency management, formatting, sanitizers, and tests.
2. Define canonical Protocol Buffer market-event schemas and validation rules.
3. Implement historical ingestion and a deterministic local event-replay path.
4. Implement leakage-safe targets and batch feature calculations in C++ with golden-data tests.
5. Implement Eigen-based Logistic Regression and walk-forward evaluation.
6. Implement the cost-aware event-driven backtester and benchmark strategies.
7. Add XGBoost training and comparison.
8. Introduce Kafka producers and consumers without changing validated domain calculations.
9. Convert feature generation to incremental, stateful stream processing and verify parity with
   batch results.
10. Add inference, risk constraints, PostgreSQL persistence, and service APIs.
11. Add failure recovery, replay, dead-letter handling, observability, and load testing.
12. Containerize the complete local system, add CI/CD, and deploy it to AWS.

## Completion and Resume Evidence

The following is the intended final resume story, not a description of the current repository.
Each statement becomes usable only after its corresponding implementation and verification exist:

- Architected an end-to-end market-signal platform in C++20, using Apache Kafka to connect
  real-time ingestion, validation, feature engineering, model training, inference, and
  risk-management services.
- Developed a stateful streaming feature engine that incrementally calculated momentum,
  volatility, volume, relative-strength, and market-regime indicators across partitioned ticker
  streams.
- Implemented leakage-safe walk-forward training and event-driven backtesting in C++, evaluating
  models across 43 or more out-of-sample folds and more than 10 years of market regimes.
- Built an Eigen-based Logistic Regression classifier with regularization, feature standardization,
  class weighting, and numerically stable optimization; benchmarked it against XGBoost baselines.
- Engineered fault-tolerant Kafka consumers with explicit offset management, idempotent processing,
  schema validation, dead-letter topics, replay support, and consumer-lag monitoring.
- Modeled transaction costs, spread, slippage, turnover, position limits, and execution delay while
  evaluating Sharpe, CAGR, Sortino, maximum drawdown, and profit factor.
- Optimized multithreaded feature and inference services to process a measured number of events per
  second at a measured p95 latency, supported by profiling and reproducible benchmarks.
- Containerized and deployed the distributed platform to AWS with automated testing, sanitizers,
  CI/CD, structured logging, and Prometheus/Grafana monitoring.

Never replace missing measurements with invented values. Record benchmark environment, dataset,
message size, partition count, concurrency, duration, and percentile methodology alongside results.

## Current Repository Status

At the time this goal was adopted, the repository contained a Python notebook with an initial IWM
research baseline and `AGENT_CONTEXT.md`. The C++ platform has not yet been implemented. Treat the
notebook as background evidence and begin the new system from the C++ foundation described above.
