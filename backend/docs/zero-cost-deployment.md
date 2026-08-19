# Zero-cost deployment

Date: 2026-08-08

Arrakis is deployed for **$0.00/month**. This document says how, and is honest
about what that costs architecturally.

It supersedes the recommendation in [`deployment-plan.md`](deployment-plan.md),
which priced always-on hosting and concluded Hetzner CX23 at $6.49/month. The
requirement changed: there is no user traffic and no latency requirement, and the
deliverable is *a signal, generated and displayed*. Under those constraints an
always-on server is pure waste. `deployment-plan.md` remains correct if the
project ever needs a live streaming API.

---

## 1. Architecture

```text
GitHub Actions (hourly, :10 past 13:00-21:00 UTC, Mon-Fri)
  ├── ephemeral Kafka 3.8 KRaft (service container, per job)
  ├── fetch-historical-data  →  refresh data/history/*.csv
  ├── daily-bar-loader       →  upsert etf_bars_daily          ──┐
  ├── news-ingestion         →  Finnhub, point-in-time XLK        │
  │                             constituents → Kafka             ├─→  Supabase
  ├── news-enricher          →  FinBERT ONNX → daily features     │    PostgreSQL
  ├── market-api (one-shot)  →  GET /news, GET /insights          │   (durable)
  └── publish_signal_supabase.sh → research_signals             ──┘
                                                                    │
Vercel static frontend ── anon key ──→ public_research_signal_latest ┘
```

Nothing runs between jobs. Kafka is created and destroyed inside each run.

### Why Supabase is the pipeline's database, not an ephemeral container

The news corpus, daily bars and feature history must accumulate across runs.
`daily_market_bars` needs a real history to compute `rsi_14`, `ret_6` and
`volatility_6`; the news tables need to persist so an hourly run is an *update*
rather than a cold start. An ephemeral database would rebuild from zero every
hour and make the warm-up problem permanent.

It also solves the free-tier pause: Supabase pauses a free project after 7 days
of inactivity, and hourly writes mean it never idles that long.

### The cron keepalive

GitHub disables scheduled workflows after **60 days of repository inactivity**.
The previous daily design committed the signal JSON, and that commit doubled as
the keepalive. Publishing to Supabase removes that commit, so the schedule would
silently lapse.

The `keepalive` job restores it: after each post-close run it commits a one-line
heartbeat to `.github/heartbeat/last-run.json`, tagged `[skip ci]`. That is
roughly one commit per trading day, far inside the 60-day window.

---

## 2. Cadence and the signal of record

The cron fires hourly from 13:10 to 21:10 UTC on weekdays, which spans the
regular session in both DST regimes (09:30-16:00 ET is 13:30-20:00 UTC under EDT
and 14:30-21:00 UTC under EST). Each run decides for itself:

| Condition (America/New_York) | Behaviour |
| --- | --- |
| Weekend | Exit cleanly, publish nothing |
| Before 09:30 | Exit cleanly, publish nothing |
| 09:30-15:59 | `run_kind = intraday`, cutoff = **now** |
| 16:00 or later | `run_kind = post_close`, cutoff = **16:00 ET** |
| No `etf_bars_daily` row for the date | Exit cleanly (market holiday or stale history) |

**Only the post-close run is the signal of record.** Every training observation
saw a full trading day of news up to the 16:00 ET close. An intraday run is
point-in-time correct — the enricher never admits an article published after its
cutoff — but it sees only part of the day, so `article_count`,
`abnormal_news_volume`, `news_coverage` and `news_freshness_hours` sit outside
the distribution the model was fitted on. The UI labels intraday documents as
provisional, and `research_signals.run_kind` records which is which.

Under EDT the 20:10 and 21:10 UTC runs are both post-close. They share the same
16:00 ET cutoff, so their feature vectors are identical; the view simply shows
the newer one.

### The lookback-window fix that this required

`news-ingestion` republishes `NEWS_POLL_LOOKBACK_DAYS` (3) of history on every
pass, and `aggregate_daily` previously filtered only on the upper cutoff. The
live `article_count` therefore counted roughly **three days** of articles against
a model trained on **one** — `build_xlk_combined_dataset` groups training
articles by their own calendar date.

`aggregate_daily` now takes an inclusive `window_start_unix_ms` lower bound
(defaulting to 0, so the batch builder is unchanged), and the workflow sets
`NEWS_PREDICTION_WINDOW_START_UNIX_MS` to midnight ET on the trading date.
Covered by `NewsAggregationTest.WindowStartBoundsTheAggregateToOneTradingDay`
and `DefaultWindowStartPreservesBatchBuilderBehaviour`.

---

## 3. Cost

| Component | Choice | Cost/month | Source |
| --- | --- | ---: | --- |
| Compute | GitHub Actions, standard runners | **$0.00** | [Actions billing](https://docs.github.com/billing/managing-billing-for-github-actions/about-billing-for-github-actions) — free and unlimited for public repositories |
| Database | Supabase Free (500 MB, 5 GB egress) | **$0.00** | [Supabase pricing](https://supabase.com/pricing) |
| Kafka | Kafka 3.8 KRaft service container, ephemeral | **$0.00** | included in the runner |
| Model artifacts | GitHub Release assets | **$0.00** | free for public repositories |
| Frontend | Vercel Hobby, static | **$0.00** | [Vercel Hobby](https://vercel.com/docs/plans/hobby) |
| **Total** | | **$0.00** | |

The only non-free dependency is **Finnhub**, already on its free tier.

**Two conditions.** The repository must stay **public** — going private moves
Actions onto the metered 2,000-minute allowance, which an hourly C++ build plus
FinBERT inference would exhaust. And the database must stay under **500 MB**.

### Storage growth is the real risk, and it is not measured

`news_nlp_features.pooled_embedding` stores a full FinBERT hidden vector as JSONB
per article. FinBERT's hidden size is 768, so one row is on the order of ~10-15 KB
of JSON text. **This is arithmetic, not a measurement** — actual row size has not
been observed, because there is no Supabase project to observe it on. At a few
hundred articles per day it plausibly reaches the 500 MB cap in months rather
than years.

`prune_research_history(retain_days)` (V009) is therefore called after every run,
defaulting to a 120-day window and deleting old `research_signals`,
`news_articles` (cascading to entities and NLP features) and
`etf_daily_news_features`. **Measure real growth after a week of running and
adjust `ARRAKIS_SIGNAL_RETAIN_DAYS`.** If embeddings dominate as expected, the
better fix is to persist only the 8 dimensions the feature schema actually
consumes rather than all 768.

### Why not the alternatives

Priced in [`deployment-plan.md`](deployment-plan.md) on 2026-08-08:

- **Hetzner CX23, $6.49/mo** — correct for an always-on API; wasted here.
- **AWS EC2 + autoscaling group scaled to 0** — does *not* reach zero. A public
  IPv4 address bills $0.005/hr = **$3.60/month** whether or not an instance is
  attached, and a persistent EBS volume bills $0.08/GB-month continuously. An ASG
  at 0 instances still costs ~$6.00-6.80/month while ingesting nothing.
- **Scheduled Fargate, ~$0.26/mo** — genuinely near-zero and the closest
  runner-up, but not free, and it still needs a registry, task definitions and a
  database.

---

## 4. What this gives up

1. **No live market WebSocket.** `market-ingestion` and `bar-aggregator` are not
   deployed; the site does not stream intraday trades. This costs less than it
   sounds — the ML path consumes daily bars, which is what the model trains on.
2. **Kafka is not continuously available.** It runs per job. The full
   producer → topic → consumer-group → offset-commit path is exercised every run,
   but there is no 24/7 broker, no sustained consumer lag, and no live replay.
3. **No Prometheus or Grafana.** `/metrics` still exists; nothing scrapes it.
4. **Freshness is bounded by the cron.** Scheduled workflows are commonly delayed
   5-30 minutes and can be dropped entirely under load, so "hourly" means
   "usually within the hour". Acceptable by design: there is no latency
   requirement, and a missed hour just means the next run covers a wider window.

### Honest resume framing

Supportable: *"Deployed a containerised C++20 market-signal pipeline — Kafka,
PostgreSQL, FinBERT ONNX inference, XGBoost — as an hourly scheduled job
producing point-in-time research documents into Supabase, at zero infrastructure
cost."*

Not supportable from this deployment: any claim of a live streaming platform,
sustained throughput, or production uptime.

---

## 5. The model gate stays closed

`ARRAKIS_XLK_NEWS_MODEL_VALIDATED` is `false` in the workflow, deliberately.

No target has cleared the pre-registered bar (validation AUC > 0.55 *and* test
AUC > 0.55 in every walk-forward window) — see
[`xlk-walk-forward-evaluation-2026-08-05.md`](xlk-walk-forward-evaluation-2026-08-05.md).
`AGENTS.md` forbids claiming predictive edge without reproducible out-of-sample
evidence.

So `/insights` returns **503 `NO_VALIDATED_MODEL`**, the published document
carries a `prediction_error` instead of a `prediction`, and the page renders its
"No validated model" panel. The pipeline still runs end to end and still
publishes the news, sentiment and feature payload — the honest deliverable today.

When a target clears the bar, flip the gate in the workflow. Not before.

---

## 6. Security surface

V006, V007 and V008 enable RLS and `REVOKE ALL` from `anon` on every table. V009
grants `SELECT` on exactly two views — `public_research_signal_latest` and
`public_research_signal_runs` — and nothing else. `research_signals` itself stays
revoked; the views execute with the owner's privileges.

Article **bodies are stripped** before publication (`export_daily_signal.sh`).
They are licensed provider text, they dominate payload size, and the UI renders
only headline, source, link and sentiment.

The anon key is a publishable credential and belongs in the bundle. The
service-role key must never appear in the frontend or in `VITE_` variables.
Writes go over `SUPABASE_DB_URL` from CI secrets, never through PostgREST.

The `supabase-migrate` workflow prints every `anon` grant after applying
migrations, so an over-broad grant shows up in the run log.

---

## 7. Setup

One-time:

1. **Create a Supabase project** (free tier). Copy the connection URI from
   Settings → Database → Connection string.
2. **Add repository secrets** (Settings → Secrets and variables → Actions):
   - `SUPABASE_DB_URL` — the Postgres URI, used for writes.
   - `FINNHUB_API_KEY`.
3. **Apply migrations**: Actions → *Apply Supabase migrations* → Run workflow →
   type `APPLY`. Re-run whenever a new `V*.sql` lands.
4. **Publish the model artifacts** to a release tagged `models-v1`:
   ```bash
   gh release create models-v1 --title "Model artifacts v1" --notes "FinBERT ONNX + tokenizer" \
     backend/models/finbert/model.onnx \
     backend/models/finbert/vocab.txt
   ```
   Override the tag with the `MODELS_RELEASE_TAG` repository variable.
5. **Connect Vercel**, root directory `frontend`, and set `VITE_SUPABASE_URL`
   and `VITE_SUPABASE_ANON_KEY` (Settings → API → Project URL and anon key).
6. **Confirm Actions can push**: Settings → Actions → General → Workflow
   permissions → *Read and write* (for the keepalive commit).

Then run *Hourly XLK signal* manually once rather than waiting for the cron.

---

## 8. What is NOT verified

Stated explicitly, per `AGENTS.md`.

- **The workflow has never executed.** There is no Docker daemon on the
  development machine and no Supabase project, so neither the service containers
  nor the SQL were run. Treat the first execution as a debugging session.
- **The V009 migration has never been applied.** The RLS/grant behaviour, the
  views and `prune_research_history` are unexecuted SQL.
- **The Debian build is unproven.** The apt list mirrors the Dockerfile's builder
  stage, but the full CMake configure/build has only ever run on macOS with Apple
  Clang. `-Werror -Wconversion -Wshadow` under GCC is the most likely first
  failure, and `libxgboost-dev` availability is second.
- **Runner resources and wall-clock are unmeasured.** FinBERT's peak RSS was
  measured at 605.6 MiB on macOS/arm64 (`deployment-plan.md` §1); Kafka's JVM on
  top of that has not been observed on a runner.
- **Supabase row sizes are unmeasured** — see §3.
- **The GNU `date -d` expressions in the workflow were not executed locally**
  (this machine has BSD `date`). The intended UTC values were confirmed
  independently: midnight ET → 04:00Z (EDT) / 05:00Z (EST), and 16:00 ET →
  20:00Z / 21:00Z.
- **`news-ingestion` and `news-enricher` are stopped with `timeout`,** because
  both are `for(;;)` daemons with no one-shot mode. The enricher commits its
  feature row before being stopped and the workflow asserts the row exists, but a
  proper `--finnhub-once` flag and an idle-exit would make this a batch job by
  design rather than by orchestration. Recommended follow-up.
- **Finnhub rate limits under the hourly cadence are untested.** A pass is ~55
  calls paced at 1.5 s; at hourly cadence that is ~440 calls per session day,
  well under the free tier's 60/minute, but sustained behaviour is unobserved.

## 9. Verified locally

- `frontend`: `npm run build` and `npm run lint` pass with the Supabase client.
- `export_daily_signal.sh` and `publish_signal_supabase.sh`: `bash -n` clean;
  the gate-closed assembly path exercised against fixtures with the real `jq`
  expressions, confirming article bodies are removed, headlines retained,
  `prediction` absent, `prediction_error` populated, and the run metadata
  stamped. Every field the publish script extracts was checked against the
  produced document.
- All three workflows parse as valid YAML.
- `aggregate_daily` window bound: 3 new GoogleTests pass, including proof the
  default preserves batch-builder behaviour.
- `bash` `test -lt` handles zero-padded `HHMM` values as decimal, so the
  session-window comparisons in the workflow are not subject to octal
  misparsing.
