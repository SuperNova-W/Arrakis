# Arrakis deployment plan (costed)

Date of pricing research: **2026-08-08**. Every dollar/euro figure below carries the URL it came
from and the region it applies to. Nothing here is estimated from memory. Where I could not fetch a
number, the document says so instead of guessing.

Design constraint accepted for this document: **there is no latency requirement.** A daily batch
cadence is sufficient for the XLK recommendation path. The optimization target is *minimum recurring
dollars*, not architectural impressiveness.

`AGENTS.md` says the platform should be "deployable to AWS". This plan takes that seriously and
prices AWS honestly against the alternatives. The conclusion is that AWS is 2-7x the cost of the
recommended option for identical capability, and that the resume claim is better served by a
*reproducible container deployment plus real CI* than by paying AWS rent. See
[AWS: what it would actually cost](#aws-what-it-would-actually-cost) for the numbers and
[Honest note on the AWS goal](#honest-note-on-the-aws-goal) for the recommendation.

---

## 1. The binding constraint: FinBERT RAM

Everything about instance sizing follows from one number, and it is measured, not assumed.

| Measurement | Value |
| --- | --- |
| `models/finbert/model.onnx` on disk | 438,204,941 bytes (418 MiB) |
| Peak RSS of `arrakis_finbert_smoke_test` with the real artifact loaded | **635,027,456 bytes (605.6 MiB)** |
| Peak memory footprint reported by the same run | 617,743,320 bytes (589.1 MiB) |

Measurement environment: macOS 26 (Darwin 25.3.0), Apple Silicon arm64, ONNX Runtime 1.28.0
(Homebrew), single `FinbertSession`, batch size 1, one 8-word sentence,
`ARRAKIS_FINBERT_MAX_TOKENS` default. Command:
`/usr/bin/time -l ./arrakis_finbert_smoke_test` with `ARRAKIS_FINBERT_ONNX_PATH` and
`ARRAKIS_FINBERT_VOCAB_PATH` set.

Caveats on that number, stated plainly:

- It is a **floor**, not a ceiling. Batch size 1 with a short sentence is the cheapest possible
  inference. Real batches of 128-token articles will allocate more activation memory.
- It was measured on macOS/arm64, not on the Linux/x86-64 target. ORT's arena allocator behaves
  similarly but this has not been re-measured on the deployment platform. **Re-measure on the box
  before finalizing the instance size.**
- Two processes that each construct a `FinbertSession` pay this cost twice. In the current
  `docker-compose.yml`, both `market-api` and `news-enricher` receive `ARRAKIS_FINBERT_ONNX_PATH`.

The single most valuable sizing decision in this plan: **only one process should hold a FinBERT
session, and it should not hold it 24/7.** That converts a ~600 MiB permanent allocation into a
~600 MiB allocation for a few minutes a day.

### Memory budget for the recommended single box

This is a *budget*, not a measurement. Only the FinBERT row is measured. The rest are planning
figures based on configured JVM heap and default Postgres settings, and must be verified with
`docker stats` after the first deploy.

| Process | Planned RSS | Basis |
| --- | --- | --- |
| Kafka (KRaft, single broker) | ~0.7-0.9 GiB | JVM with `KAFKA_HEAP_OPTS=-Xmx512m -Xms512m` plus non-heap/page-cache overhead. **Not measured.** |
| PostgreSQL 16 | ~0.2-0.3 GiB | Default `shared_buffers` 128 MB plus per-connection backends. **Not measured.** |
| `market-api` (no FinBERT session) | ~0.1-0.2 GiB | XGBoost booster + Boost.Beast + rdkafka consumer. **Not measured.** |
| `market-ingestion` | < 0.1 GiB | **Not measured.** |
| `news-ingestion` | < 0.1 GiB | **Not measured.** |
| `news-enricher` (**daily job, not resident**) | ~0.7 GiB while running | Measured FinBERT floor + rdkafka + libpq headroom. |
| OS + Docker daemon | ~0.3 GiB | **Not measured.** |
| **Steady state (enricher not running)** | **~1.4-1.9 GiB** | |
| **Peak (enricher running)** | **~2.1-2.6 GiB** | |

A 4 GB box clears the peak with roughly 35-45% headroom. A 2 GB box does not — 2 GB is the size at
which FinBERT alone consumes 30% of RAM and the JVM plus Postgres take the rest. **2 GB is
disqualified. 4 GB is the realistic minimum. 8 GB is the comfortable answer.**

---

## 2. Compute

### 2.1 Always-on VM options

All figures fetched 2026-08-08.

| Option | Spec | Price | Region | Source |
| --- | --- | --- | --- | --- |
| **Hetzner CX23** | 2 vCPU x86, 4 GB, 40 GB SSD | **€5.49/mo** (**$6.49/mo**) excl. VAT, excl. IPv4 | Germany (FSN/NBG) / Finland (HEL) | [docs.hetzner.com price-adjustment](https://docs.hetzner.com/general/infrastructure-and-availability/price-adjustment/) (effective 15 June 2026, 08:00 CEST) |
| **Hetzner CX33** | 4 vCPU x86, 8 GB, 80 GB SSD | **€8.49/mo** (**$9.99/mo**) excl. VAT, excl. IPv4 | Germany / Finland | same |
| Hetzner CAX11 | 2 vCPU Ampere ARM, 4 GB, 40 GB | €5.99/mo ($6.99/mo) | Germany / Finland only | same |
| Hetzner CAX21 | 4 vCPU Ampere ARM, 8 GB, 80 GB | €10.49/mo ($12.49/mo) | Germany / Finland only | same |
| Oracle Cloud Always Free (Ampere A1) | 2 OCPU ARM, 12 GB | **$0.00/mo** | Any OCI home region with A1 capacity | see §2.2 |
| Fly.io `shared-cpu-1x`, 2 GB | 1 shared vCPU, 2 GB | $11.11/mo + volumes | Amsterdam | [fly.io/docs/about/pricing](https://fly.io/docs/about/pricing/) |
| AWS Lightsail 4 GB | 2 vCPU, 4 GB, 80 GB SSD, 4 TB transfer | **$24.00/mo** | us-east-1 | [aws.amazon.com/lightsail/pricing](https://aws.amazon.com/lightsail/pricing/) |
| AWS Lightsail 8 GB | 2 vCPU, 8 GB, 160 GB SSD, 5 TB transfer | $44.00/mo | us-east-1 | same |
| AWS EC2 `t4g.small` | 2 vCPU ARM, 2 GB | $0.017/hr = **$12.41/mo** compute | us-east-1 | [instances.vantage.sh/aws/ec2/t4g.small](https://instances.vantage.sh/aws/ec2/t4g.small) |
| — plus in-use public IPv4 | | $0.005/hr = **$3.65/mo** | us-east-1 | [aws.amazon.com/vpc/pricing](https://aws.amazon.com/vpc/pricing/) |
| — plus EBS root volume | | not priced here | | |

Specs for the Hetzner CX/CAX lines are from
[hetzner.com/cloud/cost-optimized](https://www.hetzner.com/cloud/cost-optimized/) (fetched
2026-08-08); that page renders prices client-side, so the prices above come from the docs page
instead. Hetzner's ARM (CAX) instances are **not available in Ashburn or Hillsboro** — EU regions
only.

Three things fall out of this table:

1. **After the 15 June 2026 price adjustment, Hetzner's x86 line is cheaper than its ARM line at
   equal RAM** (CX33 €8.49 vs CAX21 €10.49; CX23 €5.49 vs CAX11 €5.99). ARM's usual price argument
   is gone here, and x86 avoids having to source `aarch64` builds of `onnxruntime`, `xgboost`, and
   `librdkafka`. **Choose x86.** This is a real reversal from the pre-June-2026 situation and worth
   re-checking if Hetzner adjusts again.
2. `t4g.small` is 2 GB, which §1 disqualifies. The honest AWS EC2 comparison is `t4g.medium` (4 GB),
   which I did not fetch a price for; at minimum it is more than the $16.06/mo that `t4g.small` +
   IPv4 already costs before storage.
3. Lightsail 4 GB at $24.00/mo is **3.7x the price of the equivalent Hetzner CX23** for the same
   4 GB of RAM. The 4 TB transfer allowance is irrelevant to a workload that serves a handful of
   JSON documents a day.

The IPv4 surcharge on Hetzner is real but was **not published on either page I fetched** (the docs
price table is explicitly "excl. IPv4"). Budget for it separately and confirm in the Hetzner
console; do not treat €5.49 as the final invoice.

### 2.2 Oracle Cloud Always Free — $0, with a serious asterisk

Oracle **halved** the Always Free Ampere A1 allowance from 4 OCPU / 24 GB to **2 OCPU / 12 GB**,
effective 15 June 2026, with **no blog post, no changelog, and no customer notification** — users
found out when instances were shut down. Oracle has since emailed Always Free users that instances
above the new limits **will be terminated on or after 18 August 2026** — ten days after this
document was written.

Sources: [InfoQ, July 2026](https://www.infoq.com/news/2026/07/oracle-cloud-free-tier-limits/),
[Linuxiac](https://linuxiac.com/oracle-quietly-cuts-free-tier-ampere-a1-resources-in-half/),
[TerminalBytes](https://terminalbytes.com/oracle-cloud-free-tier-changes-2026/) (all fetched via
search 2026-08-08; Oracle's own free-tier page returned HTTP 403 to my fetcher, so I could not
confirm this against a first-party source — **verify in the OCI console before committing**).

2 OCPU / 12 GB is still more than enough for this stack, and $0 is unbeatable on the stated
constraint. But the failure mode is "your portfolio demo is down when a recruiter opens it, with no
notice", compounded by OCI's well-documented "Out of capacity" errors when creating A1 shapes. That
is why it is the runner-up and not the recommendation.

### 2.3 ECS Fargate — and the scheduled-job insight

Fargate rates, us-east-1, [aws.amazon.com/fargate/pricing](https://aws.amazon.com/fargate/pricing/),
fetched 2026-08-08:

| | vCPU-hour | GB-hour |
| --- | --- | --- |
| Linux/x86 | $0.0404784 | $0.004446 |
| Linux/ARM | $0.0323798 | $0.0035600 |

**Always-on**, one ARM task sized 1 vCPU / 4 GB (the minimum that clears §1's FinBERT floor with
headroom):

```
(0.0323798 + 4 × 0.0035600) × 730 h = 0.0466198 × 730 = $34.03 / month / task
```

That is one task. The stack has five services. Before Kafka. Before Postgres. Before the ALB.
**Fargate as a persistent-service platform is disqualified at ~$170+/mo of tasks alone.**

**Scheduled**, the same task run for 15 minutes on each of ~22 trading days:

```
0.0466198 × 0.25 h × 22 = $0.26 / month
```

**A 130x difference for identical compute.** This is the single most important cost lever in the
document, and it is not Hetzner-specific: *the enrichment path should be a scheduled job on any
platform.* On the recommended VM it is a `systemd` timer; on AWS it would be an EventBridge
Scheduler rule firing an ECS `RunTask`. Either way, `news-enricher` must not be a `restart:
unless-stopped` service in the deployed profile the way it is in `docker-compose.yml` today.

### 2.4 Compute verdict

**One Hetzner CX23 (2 vCPU / 4 GB / 40 GB, €5.49/mo excl. VAT and IPv4, Falkenstein or Helsinki),
running the whole stack under Docker Compose, with `news-enricher` converted from a resident service
to a daily `systemd` timer.**

Upgrade path if `docker stats` shows the §1 budget was optimistic: **CX33** at €8.49/mo doubles RAM
and CPU for €3.00/mo more. Take that upgrade without agonizing — it is still a third of Lightsail.

---

## 3. Kafka

`AGENTS.md` requires Kafka to be substantive: partitioning by symbol, consumer groups, explicit
offset management, DLQ topics, replay, consumer-lag metrics. Dropping Kafka is not on the table.
The question is only where the broker runs.

### 3.1 Managed Kafka pricing

All us-east-1, fetched 2026-08-08 from
[aws.amazon.com/msk/pricing](https://aws.amazon.com/msk/pricing/) and
[confluent.io/confluent-cloud/pricing](https://www.confluent.io/confluent-cloud/pricing/).

| Option | Unit price | Monthly floor for this workload |
| --- | --- | --- |
| **MSK Serverless** | $0.75/cluster-hour + $0.0015/partition-hour + $0.10/GB in + $0.05/GB out + $0.10/GB-mo storage | $0.75 × 730 = **$547.50** base, plus 18 partitions × $0.0015 × 730 = **$19.71** → **≥ $567/mo** |
| **MSK Provisioned** (`kafka.m7g.large`) | $0.204/broker-hour + $0.10/GB-mo storage | $0.204 × 730 = **$148.92/mo per broker**, before storage, before the second broker any real MSK deployment wants |
| MSK Provisioned (`kafka.m5.large`) | $0.21/broker-hour | $153.30/mo per broker |
| MSK Express (`express.m7g.large`) | $0.408/broker-hour + $0.01/GB ingest | $297.84/mo per broker |
| **Confluent Cloud Basic** | **first eCKU free**, then $0.14/eCKU-hour; $0.05/GB ingress *and* egress; $0.08/GB-month storage | **~$0-2/mo** at this throughput |

The 18-partition figure is the current topic layout in `docker-compose.yml`: three topics
(`market.raw.trades`, `news.raw.articles`, `news.enriched.features`) × 6 partitions.

**MSK is disqualifying by a factor of 25 to 90 against the entire rest of the stack.** MSK Serverless
alone would cost more per month than the recommended architecture costs per year, several times over.

**Confluent Cloud Basic is the one managed option that is not disqualifying**, and this document
should say so rather than pretend otherwise. With the first eCKU free it is plausibly $0-2/mo at
this volume. It is still not the recommendation, for three specific reasons:

1. **Metered egress punishes exactly the behaviour AGENTS.md asks you to demonstrate.** Historical
   replay and deterministic state rebuilding are billable read traffic at $0.05/GB. An afternoon of
   debugging a replay path is a line item.
2. **It hides the ops surface that is the point of the exercise.** Broker configs, KRaft controller
   quorum, retention tuning, forced-restart recovery, rebalance behaviour under a killed consumer —
   these are the resume claims. On Basic you get a topic API.
3. **It requires a credit card on a metered service with no hard spend cap**, which is precisely the
   risk profile the owner asked to avoid.

### 3.2 Single-broker KRaft on the same VM

This is what `docker-compose.yml` already runs (`bitnamilegacy/kafka:3.8`, `KAFKA_CFG_PROCESS_ROLES:
controller,broker`, single node, `replication-factor 1`). It needs **no change to run in
production** beyond heap tuning and retention.

**Dollar cost: $0.00.** It is a container on a VM already paid for.

**RAM cost:** the JVM heap you give it, plus JVM overhead. Set it explicitly in the deployed profile:

```yaml
KAFKA_HEAP_OPTS: "-Xmx512m -Xms512m"
```

Planning figure ~0.7-0.9 GiB RSS including non-heap overhead. **I did not measure this** — no
container runtime was exercised for this document. Verify with `docker stats kafka` after deploy and
adjust `-Xmx` down toward 384m if there is room, or up if the broker GCs under replay load.

Retention is already sane at `KAFKA_CFG_LOG_RETENTION_HOURS: 72`. On a 40 GB disk that is
comfortable for this event volume, but add a size-based bound as a belt-and-braces guard against a
runaway producer filling the root filesystem and taking Postgres down with it:

```yaml
KAFKA_CFG_LOG_RETENTION_BYTES: 2147483648   # 2 GiB per partition ceiling
```

### 3.3 Kafka verdict

**Single-broker KRaft on the same VM. $0.00/mo, ~0.8 GiB RAM.** It satisfies every Kafka requirement
in `AGENTS.md` except multi-broker replication — and `replication-factor 1` is already what the
local stack runs, so the deployment introduces no new dishonesty. Document the delivery semantics
as at-least-once with RF=1 and no exactly-once claim, exactly as `README.md` already does.

---

## 4. PostgreSQL

| Option | Free-tier limits | Verdict |
| --- | --- | --- |
| **Supabase Free** | 500 MB database (shared CPU, 500 MB RAM), 5 GB egress, 1 GB file storage, 50k MAU, **max 2 active projects**, **paused after 1 week of inactivity**. Pro is $25/mo. [supabase.com/pricing](https://supabase.com/pricing) | Idle-pause is **not** the problem — a daily write keeps it active. The **500 MB cap** is. |
| **Neon Free** | 0.5 GB storage/project, **100 CU-hours/project/month**, 5 GB egress, autosuspend after 5 min idle, autoscale ceiling 2 CU. Next tier "Launch" at $0.106/CU-hour. [neon.com/pricing](https://neon.com/pricing) | **Structurally broken for this workload.** See below. |
| **Postgres 16 container on the same VM** | Bounded only by the 40 GB disk | **Recommended. $0.00/mo.** |

### The Neon trap — the sharpest cost trap in this document

Neon's free tier is metered in **compute-hours, not just storage**. A month is 730 hours. The free
allowance is 100 CU-hours per project.

`market-api` holds a libpq connection for its ML/news routes and `bar-aggregator` writes
continuously. **A service that maintains a persistent connection keeps Neon's compute awake, and
awake compute burns CU-hours 24/7.** Even at the smallest autoscaling size, 730 hours of uptime
cannot fit inside 100 CU-hours: at 0.25 CU that is 182.5 CU-hours, roughly 1.8x the allowance, and
the free tier is exhausted around day 16. (Neon's page documents the 2 CU *ceiling*; it does not
document the floor, so treat 0.25 CU as unverified — but the conclusion holds at any floor ≥ 0.14
CU.)

The failure is quiet: the project does not obviously break, it just starts costing $0.106/CU-hour,
and you notice on the invoice. **This is the trap to flag loudest**, because "Neon has a free tier"
is true and "Neon is free for this workload" is false.

Supabase's 500 MB cap is the second-order version of the same problem. `bar-aggregator` persists
1-minute bars for the 18-symbol universe in `config/etf_universe.json`. Order-of-magnitude: 18
symbols × 390 minutes × ~250 trading days ≈ 1.75M rows/year, which with indexes lands in the
hundreds of MB — the same order as the 500 MB ceiling. It will not break in month one; it will break
somewhere in year one, and then you are on Pro at $25/mo, which is **four times the entire
recommended stack**. (This row-count arithmetic is arithmetic, not a measurement — actual row width
has not been measured against the migrations.)

### PostgreSQL verdict

**`postgres:16-alpine` on the same VM, backed by a named Docker volume, $0.00/mo.** Disk is the only
limit and 40 GB is ~80x the Supabase free cap.

Cost of doing this: you own backups. Mitigation that keeps the bill at zero — a nightly
`pg_dump | gzip` uploaded to the same Cloudflare R2 bucket used for the model (§5), well inside R2's
10 GB-month free tier.

`SUPABASE_DB_URL` should remain empty. Keep the `.env.example` comment and the `README.md` note
about the Supabase migration path — it is a legitimate documented escape hatch if the box ever needs
to shed the database — but do not adopt it now.

---

## 5. Model artifact hosting (438 MB FinBERT ONNX)

First, a correction to a common assumption about this repo: **`models/finbert/model.onnx` is not
checked into git.** `backend/.gitignore` contains `/models/finbert/*` with `!` exceptions only for
`README.md` and `manifest.json`, and `git ls-files models/` returns exactly those two files. The
438 MB file exists only in the local working tree. There is no repo bloat to clean up — the artifact
simply has no home yet, which is why `ARRAKIS_FINBERT_MODEL_URL` is empty.

| Option | Cost for 438 MB + 226 KB | Notes |
| --- | --- | --- |
| **Cloudflare R2** | **$0.00** | Free tier: 10 GB-month storage, 1M Class A ops, 10M Class B ops/month; **egress free**. [developers.cloudflare.com/r2/pricing](https://developers.cloudflare.com/r2/pricing/) |
| Amazon S3 Standard | ~$0.01/mo | $0.023/GB-month × 0.44 GB. Storage rate from secondary sources ([CloudZero](https://www.cloudzero.com/blog/s3-pricing/)) — the AWS pricing page renders rates in JS tables my fetcher could not read. First 100 GB/month of internet egress is free across all AWS services, which I *did* confirm on [aws.amazon.com/s3/pricing](https://aws.amazon.com/s3/pricing/). |
| Hugging Face Hub | $0.00, **best-effort** | Free public storage is explicitly "**Best-effort**" with anti-abuse mitigations and a stated expectation that uploads be "as useful to the community as possible". [huggingface.co/docs/hub/storage-limits](https://huggingface.co/docs/hub/storage-limits) |
| Bake into the container image | $0.00 on GHCR (public repo) | 438 MB in every image layer, re-pushed and re-pulled on every deploy, and model version welded to image version. |

### An honest finding: R2's free egress is irrelevant here

R2's headline advantage over S3 is zero egress fees. **At this volume that advantage does not
apply.** The model is pulled maybe a handful of times a month (initial provision, redeploys, CI is
explicitly excluded). Even 200 pulls/month is 88 GB — still inside AWS's free 100 GB/month internet
egress allowance. R2 wins here for a different and more boring reason: **its free tier makes the
bill exactly $0.00 with no AWS account, no billing alarm, and no chance of an accidental charge.**
Say that, rather than repeating the egress talking point where it does not bite.

Hugging Face is rejected for a specific reason, not a vague one: the artifact is a *custom Arrakis
export* of a ProsusAI derivative whose contract (`manifest.json`: logits ordered positive/negative/
neutral, plus a pooled-embedding output) is deliberately **not** interchangeable with public FinBERT
exports. It has near-zero community value, which is exactly what HF's best-effort policy asks you not
to upload. A deploy-critical dependency should not sit on storage the host reserves the right to
reclaim.

Baking into the image is rejected because it defeats the checksum-pinned fetch design already built
in `scripts/fetch_finbert_model.sh` and turns every model rev into an image rebuild.

### Concrete change

`scripts/fetch_finbert_model.sh` uses plain `curl --fail --location` with **no request signing**.
This has a hard consequence: **the object must be publicly readable over HTTPS.** The script cannot
do SigV4, so a private R2 or S3 bucket will 403. Integrity is protected by the SHA-256 values pinned
in `manifest.json` and defaulted in the script
(`4a8d58ba…` for the model, `07eced37…` for the vocab), which is the correct design — a public
object with a pinned checksum is safe.

Steps:

1. Create an R2 bucket, e.g. `arrakis-artifacts`.
2. Upload under a **versioned prefix** — never overwrite in place:
   ```sh
   # backend/
   npx wrangler r2 object put arrakis-artifacts/finbert-v1/model.onnx --file models/finbert/model.onnx
   npx wrangler r2 object put arrakis-artifacts/finbert-v1/vocab.txt  --file models/finbert/vocab.txt
   ```
3. Expose it. Either enable the bucket's public `r2.dev` development URL (simplest; Cloudflare
   rate-limits `r2.dev` and does not recommend it for production traffic — fine for a handful of
   pulls a month), or attach a custom domain such as `artifacts.<yourdomain>` (recommended if you
   own a domain; removes the rate limit and gives a stable URL).
4. Set in the **deployment** `.env` — not in `.env.example`, which stays empty:
   ```sh
   ARRAKIS_FINBERT_MODEL_URL=https://artifacts.example.com/finbert-v1/model.onnx
   ARRAKIS_FINBERT_VOCAB_URL=https://artifacts.example.com/finbert-v1/vocab.txt
   ```
5. Run `./scripts/fetch_finbert_model.sh` on the box during provisioning, into a directory mounted
   as a Docker volume so the 438 MB download survives `docker compose down`. Leave
   `ARRAKIS_FINBERT_MODEL_SHA256` / `ARRAKIS_FINBERT_VOCAB_SHA256` unset so the script uses the
   manifest-pinned defaults — overriding them is how you accidentally accept a wrong artifact.

No change to the script itself is required. It was written for exactly this and only needs its two
URL variables populated.

**Backups use the same bucket at the same $0.00** (§4). 10 GB-month covers the 0.44 GB artifact plus
a rolling window of gzipped `pg_dump` files with a wide margin.

---

## 6. Frontend

`frontend/vercel.json` already exists with the correct Vite framework preset, `dist` output, and the
SPA rewrite. Deploying is a `git push`.

**Vercel Hobby: $0.00/mo, confirmed free and adequate.**
[vercel.com/docs/plans/hobby](https://vercel.com/docs/plans/hobby) (fetched 2026-08-08): 1,000,000
edge requests, 100 deployments/day, 200 projects, 2 vCPU / 8 GB build machines. This project is a
static Vite SPA — it invokes no Vercel Functions, so the Active-CPU and Provisioned-Memory
allowances (4 CPU-hrs, 360 GB-hrs) are not consumed at all.

**One condition, stated because it is a real constraint and not a formality:** the Hobby plan
"restricts users to non-commercial, personal use only" per Vercel's fair-use guidelines. A personal
portfolio project that makes no revenue and executes no trades qualifies. If the project ever
monetizes, this becomes $20/seat/month on Pro.

**Runner-up: Cloudflare Pages, also $0.00.** Per
[developers.cloudflare.com/pages/functions/pricing](https://developers.cloudflare.com/pages/functions/pricing/)
(fetched 2026-08-08), "requests to static assets are free and unlimited" on the free plan when they
do not invoke Functions — which is exactly this site. Pages carries no non-commercial restriction,
which makes it the better long-term home; but Vercel is zero-work today because `vercel.json` is
already committed. **Take Vercel now; Cloudflare Pages is a 20-minute migration if the
non-commercial clause ever matters.**

Required config change either way: set `MARKET_UI_API_BASE_URL` at build time to the API's real
public origin (see §8).

---

## 7. Observability

Current `docker-compose.yml` runs `prom/prometheus:latest` and `grafana/grafana:latest`, plus
`provectuslabs/kafka-ui:latest`.

On a 4 GB box, three extra JVM/Go containers compete for RAM with the thing that actually produces
the product. Prometheus additionally grows a TSDB on the same 40 GB disk that Kafka's 72-hour
retention and Postgres are already sharing. **I have not measured the RSS of any of these three
containers**, so I will not put a number on what they cost — but the structural argument does not
need one: their combined footprint is not zero, the box has ~1.5-2 GiB of headroom, and a
single-user portfolio project does not need a self-hosted metrics stack resident 24/7.

**Recommendation: drop `prometheus`, `grafana`, and `kafka-ui` from the deployed profile.**

Concretely, without editing the existing file (another agent owns it): add a
`docker-compose.deploy.yml` override, or move the three services behind a Compose profile
(`profiles: ["observability"]`) so `docker compose up` on the server starts only the runtime
services while local development keeps the full stack. Either approach leaves the local developer
experience exactly as it is today, which is the point — `AGENTS.md` asks for Prometheus and Grafana,
and the local stack continues to deliver them.

**Keep unconditionally:** the `/metrics` endpoint in `market-api`, the `/health` and `/ready`
endpoints, and the existing healthcheck. The instrumentation is the engineering claim; the resident
scrapers are not.

**If you want a hosted dashboard, Grafana Cloud Free is $0.00** and remains honest about the
Prometheus/Grafana requirement: 10k active series, 50 GB logs, 14-day retention, 3 users
([grafana.com/pricing](https://grafana.com/pricing/), fetched 2026-08-08). This project will not
approach 10k series. Point a lightweight Grafana Agent / Alloy at `market-api:8080/metrics` and push
remotely — you get dashboards and alerting for free, off-box, and you can screenshot them for the
portfolio without paying RAM rent for Grafana 24/7.

---

## 8. Recommendation and bottom line

### Recommended stack — **~$6.49/month**

| Component | Choice | Monthly |
| --- | --- | --- |
| Compute | Hetzner **CX23** (2 vCPU, 4 GB, 40 GB), Falkenstein or Helsinki | **€5.49 / $6.49** excl. VAT, excl. IPv4 |
| Kafka | Single-broker KRaft container, same VM, `-Xmx512m` | $0.00 |
| PostgreSQL | `postgres:16-alpine` container, same VM, named volume | $0.00 |
| Model artifact | Cloudflare R2 `arrakis-artifacts/finbert-v1/`, public + SHA-256 pinned | $0.00 (free tier) |
| DB backups | nightly `pg_dump.gz` → same R2 bucket | $0.00 (free tier) |
| Frontend | Vercel Hobby (`vercel.json` already committed) | $0.00 |
| Observability | `/metrics` retained; scrapers dropped from deploy profile; Grafana Cloud Free optional | $0.00 |
| CI | GitHub Actions, standard runners, **public repo** | $0.00 |
| **Total** | | **≈ $6.49/mo, ≈ $78/yr** |

Add the Hetzner IPv4 surcharge and VAT, both of which I could not source; realistically **under
$9/month all-in**. Upgrading to CX33 (8 GB) if the §1 memory budget proves optimistic takes it to
**€8.49 / $9.99**, still under half of Lightsail's 4 GB plan.

**Zero promotional cloud credits are consumed.** Nothing in this stack is on a trial clock.

### Runner-up — **$0.00/month**

**Oracle Cloud Always Free**, Ampere A1, 2 OCPU / 12 GB, everything else identical (R2, Vercel,
GitHub Actions all remain free). Genuinely $0.00/month and *more* RAM than the recommendation.

Chosen against because of demonstrated provider behaviour, not ideology: Oracle halved the A1 free
allowance on 15 June 2026 with no announcement and is terminating non-conforming instances from
18 August 2026. Combined with chronic "Out of capacity" errors when provisioning A1 shapes and no
SLA, the expected value of "$6.49/month for a box that stays up" is higher than "$0.00/month for a
box that might not." It also requires `aarch64` builds of `onnxruntime`, `xgboost`, and `librdkafka`
— buildable, but unvalidated work this repo has not done.

**Take the runner-up if** the €5.49 genuinely matters more than uptime, or if you want a free warm
standby: provision the OCI instance, run the same Compose file, and keep it as the failover target.

### Explicitly rejected

- **MSK Serverless — ≥ $567/mo.** 87x the recommended total stack.
- **MSK Provisioned — $148.92/mo per broker.** 23x.
- **Fargate as persistent services — $34.03/mo per 1 vCPU / 4 GB ARM task**, ~$170/mo for five,
  before Kafka, Postgres, and load balancing.
- **Lightsail 4 GB — $24/mo.** Same RAM as CX23 at 3.7x the price.
- **Neon Free.** Not actually free for a workload with a persistent connection (§4).
- **Supabase Pro — $25/mo.** Four times the entire recommended stack, to escape a 500 MB cap that a
  $0 container on an already-paid-for disk does not have.

---

## Honest note on the AWS goal

`AGENTS.md` lists AWS as the deployment target and the resume line reads "Containerized and deployed
the distributed platform to AWS with automated testing, sanitizers, CI/CD, structured logging, and
Prometheus/Grafana monitoring."

The cheapest configuration that honestly satisfies that sentence is roughly **Lightsail 4 GB at
$24/mo** (or `t4g.medium` + IPv4 + EBS, which I did not fully price but which exceeds $16/mo before
storage) — call it **$288/year for a wording change**, since the container images, Compose file,
Kafka topology, migrations, and CI are byte-identical on either host.

Two ways to reconcile this without spending $288/year and without overclaiming:

1. **Preferred.** Deploy to Hetzner. Adjust the resume line to what is verifiably true —
   *"Containerized the platform with Docker Compose and deployed it to a single cloud VM with CI,
   structured logging, and Prometheus metrics"* — and make the **CI workflow, the checksum-pinned
   artifact fetch, the KRaft broker ops, and the walk-forward evaluation** the substance of the
   claim. Those are what a competent interviewer probes. Nobody has ever been rejected for hosting a
   portfolio project on the wrong VPS; plenty have been caught out claiming AWS depth they could not
   describe.
2. **If the AWS line is non-negotiable**, buy the cheapest *honest* version of it: Lightsail 4 GB at
   $24/mo, same Compose file, and additionally use the free tier of ECR, plus EventBridge Scheduler
   → ECS `RunTask` for the daily enricher (§2.3, $0.26/mo) so there is at least one genuinely
   AWS-native, genuinely cost-motivated design decision to talk about. Do not use MSK.

**Recommendation: option 1.** Update `AGENTS.md`'s deployment target to reflect the decision, and
record the cost reasoning — an explicit, costed "we chose not to use AWS and here is the arithmetic"
is a stronger engineering artifact than an AWS deployment nobody asked about.

---

## 9. Migration steps

Ordered, with the specific config landmines called out.

### 9.1 Secrets — do this first

**`POSTGRES_PASSWORD` currently defaults to a checked-in value.** `.env.example` line 10 sets
`POSTGRES_PASSWORD=arrakis-local-dev-only`, and `docker-compose.yml` falls back to that literal in
five places (`${POSTGRES_PASSWORD:-arrakis-local-dev-only}` on `postgres`, `postgres-migrations`,
`bar-aggregator`, `market-api`, `news-enricher`). The comment correctly labels it Compose-only, and
the fallback exists so `docker compose config` works without a private file — that design is fine
locally and **must not survive contact with a public IP.**

On the server:

```sh
umask 077
cp .env.example .env
printf 'POSTGRES_PASSWORD=%s\n' "$(openssl rand -base64 32 | tr -d '/+=' | cut -c1-32)" >> .env
```

`.env` is already in `backend/.gitignore`. Verify before first boot:

```sh
docker compose config | grep -c 'arrakis-local-dev-only'   # must print 0
```

Also required in the deployment `.env`:

- `FINNHUB_API_KEY` — the live stream and news poller need it; without it the API still serves
  persisted bars.
- `APP_ENV=production`, `LOG_LEVEL=info`.
- **Do not** expose `5432` to the internet. The published `ports: - "5432:5432"` in the Compose file
  is for local development. In the deploy override, drop that mapping (and Kafka's `9092`/`9094`)
  so only `market-api` is reachable. Add a host firewall (`ufw allow 22,80,443/tcp`, default deny)
  as the second layer.

### 9.2 Origins — both of these are currently localhost and will silently break the UI

`CORS_ALLOWED_ORIGINS` must be the **exact browser origin**, scheme and host, no trailing slash.
`.env.example` line 19 has `http://localhost:3000`. `docker-compose.yml` line 136 uses it as the
`market-api` fallback. Deployed value:

```sh
CORS_ALLOWED_ORIGINS=https://arrakis.vercel.app        # or your custom domain
```

If you serve the UI on both `https://arrakis.vercel.app` and a custom domain, both origins must be
listed — a mismatch produces a silent CORS failure in the browser with a perfectly healthy `/health`
on the server, which is a genuinely annoying thing to debug.

`MARKET_UI_API_BASE_URL` (`.env.example` line 17, currently `http://localhost:8080`) is consumed at
**Vite build time**, so it must be set in Vercel's project environment variables, not in the
server's `.env`:

```sh
MARKET_UI_API_BASE_URL=https://api.arrakis.example.com
```

**It must be `https://`.** Vercel serves the UI over HTTPS, and a browser will block mixed-content
`http://` XHR and WebSocket (`ws://`) connections from an HTTPS page. This forces §9.3.

### 9.3 TLS on the API

Because §9.2 forces HTTPS, put Caddy or nginx in front of `market-api` on the box and terminate TLS
with a free Let's Encrypt certificate. Caddy is the least effort — a two-line `Caddyfile` gets
automatic certificate issuance and renewal:

```
api.arrakis.example.com {
    reverse_proxy market-api:8080
}
```

**Cost: $0.00**, plus whatever a domain costs (a domain is optional if you accept a `*.vercel.app`
frontend, but the API needs a hostname for a certificate, so plan on owning one).

The WebSocket route `/ws/v1/market` must be proxied too; Caddy's `reverse_proxy` handles the
upgrade automatically.

### 9.4 Deployment sequence

1. Provision CX23, Ubuntu LTS, SSH key only, disable password auth.
2. Install Docker Engine + Compose plugin. `ufw` default deny inbound; allow 22/80/443.
3. Clone the repo. Write `.env` per §9.1/§9.2.
4. Point `ARRAKIS_FINBERT_MODEL_URL` / `ARRAKIS_FINBERT_VOCAB_URL` at R2 (§5) and run
   `./scripts/fetch_finbert_model.sh` into a path mounted as a Docker volume.
5. Add the deploy override (`docker-compose.deploy.yml`) that: drops `prometheus`, `grafana`,
   `kafka-ui`; removes the `5432`, `9092`, `9094` host port mappings; sets
   `KAFKA_HEAP_OPTS=-Xmx512m -Xms512m` and `KAFKA_CFG_LOG_RETENTION_BYTES`; and changes
   `news-enricher` from `restart: unless-stopped` to a one-shot (§2.3).
6. `docker compose -f docker-compose.yml -f docker-compose.deploy.yml up -d --build`. Confirm
   `postgres-migrations` completed and `/ready` returns healthy.
7. **`docker stats` — validate §1's memory budget against reality and record the numbers.** If the
   steady state exceeds ~2.5 GiB, resize to CX33 before tuning anything else.
8. `systemd` timer for `news-enricher`, scheduled after the US market close cutoff the enricher
   already derives (America/New_York close) — e.g. `OnCalendar=Mon-Fri 21:30 UTC`.
9. `systemd` timer for the nightly `pg_dump` → R2 (§4).
10. Caddy in front of `market-api` (§9.3). Verify the certificate and that
    `curl https://api.../health` succeeds from off-box.
11. Deploy the frontend to Vercel with `MARKET_UI_API_BASE_URL` set in project env vars. Confirm no
    CORS errors in the browser console — this is the step that most often fails first.
12. Optional: Grafana Alloy → Grafana Cloud Free (§7).

### 9.5 Keep unchanged

- `ARRAKIS_XLK_NEWS_MODEL_VALIDATED=false` until a target actually clears the walk-forward bar
  documented in `docs/xlk-walk-forward-evaluation-2026-08-05.md`. Deploying does not change what the
  evidence supports; the API's fail-closed behaviour on the news routes is correct and should ship
  as-is.
- `SUPABASE_DB_URL` empty (§4).
- `ARRAKIS_FINBERT_MODEL_URL` / `ARRAKIS_FINBERT_VOCAB_URL` empty **in `.env.example`** — they are
  deployment values, not defaults.

---

## Sources

All fetched 2026-08-08 unless noted. Regions as stated per line.

- [docs.hetzner.com — price adjustment, effective 15 June 2026](https://docs.hetzner.com/general/infrastructure-and-availability/price-adjustment/) — CX/CAX monthly prices, Germany (FSN/NBG) / Finland (HEL), excl. VAT and IPv4
- [hetzner.com/cloud/cost-optimized](https://www.hetzner.com/cloud/cost-optimized/) — CX/CAX vCPU/RAM/disk specs
- [aws.amazon.com/lightsail/pricing](https://aws.amazon.com/lightsail/pricing/) — Lightsail Linux bundles, us-east-1
- [instances.vantage.sh/aws/ec2/t4g.small](https://instances.vantage.sh/aws/ec2/t4g.small) — EC2 t4g.small on-demand, us-east-1 (third-party mirror of AWS pricing)
- [aws.amazon.com/vpc/pricing](https://aws.amazon.com/vpc/pricing/) — in-use public IPv4 $0.005/hr
- [aws.amazon.com/fargate/pricing](https://aws.amazon.com/fargate/pricing/) — Fargate x86 and ARM vCPU/GB-hour, us-east-1
- [fly.io/docs/about/pricing](https://fly.io/docs/about/pricing/) — shared-cpu-1x, volumes, egress, Amsterdam
- [aws.amazon.com/msk/pricing](https://aws.amazon.com/msk/pricing/) — MSK Provisioned, Express, and Serverless, us-east-1
- [confluent.io/confluent-cloud/pricing](https://www.confluent.io/confluent-cloud/pricing/) — Basic cluster eCKU, ingress/egress, storage
- [supabase.com/pricing](https://supabase.com/pricing) — Free plan limits and pause behaviour; Pro $25/mo
- [neon.com/pricing](https://neon.com/pricing) — Free plan storage, CU-hours, autosuspend; Launch $0.106/CU-hour
- [developers.cloudflare.com/r2/pricing](https://developers.cloudflare.com/r2/pricing/) — R2 storage, ops, free egress, free tier
- [aws.amazon.com/s3/pricing](https://aws.amazon.com/s3/pricing/) — confirmed first 100 GB/month internet egress free; per-GB rates not machine-readable on the page
- [cloudzero.com/blog/s3-pricing](https://www.cloudzero.com/blog/s3-pricing/) — **secondary source** for S3 Standard $0.023/GB-month, us-east-1
- [huggingface.co/docs/hub/storage-limits](https://huggingface.co/docs/hub/storage-limits) — free public storage is "best-effort"
- [vercel.com/docs/plans/hobby](https://vercel.com/docs/plans/hobby) — Hobby allowances and non-commercial restriction
- [developers.cloudflare.com/pages/functions/pricing](https://developers.cloudflare.com/pages/functions/pricing/) — static assets free and unlimited on the free plan
- [grafana.com/pricing](https://grafana.com/pricing/) — Grafana Cloud Free: 10k series, 50 GB logs, 14-day retention, 3 users
- [docs.github.com — about billing for GitHub Actions](https://docs.github.com/en/billing/managing-billing-for-your-products/about-billing-for-github-actions) — free for public repos; private-repo minute rates
- Oracle Always Free June 2026 reduction — [InfoQ](https://www.infoq.com/news/2026/07/oracle-cloud-free-tier-limits/), [Linuxiac](https://linuxiac.com/oracle-quietly-cuts-free-tier-ampere-a1-resources-in-half/), [TerminalBytes](https://terminalbytes.com/oracle-cloud-free-tier-changes-2026/). **Oracle's own free-tier page returned HTTP 403 to my fetcher; no first-party confirmation was obtained.**

Figures I could **not** source and which are therefore absent from the totals: the Hetzner primary
IPv4 monthly surcharge, EBS gp3 per-GB-month, the EC2 `t4g.medium` hourly rate, and Cloudflare Pages
monthly build limits.
