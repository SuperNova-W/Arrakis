# XLK XGBoost retraining — 2026-08-10

This is a reproducible CPU retraining record. The active `xlk_news_xgboost.json`
artifact was not replaced or promoted.

## Dataset rebuild

The frozen FinBERT ONNX session was rerun over the normalized FNSPID corpus after
filtering each article to `published_at_utc <= market close` for its assigned
trading date. The resulting combined dataset is:

- Path: `data/fnspid/normalized/xlk_combined_features_finbert_rebuilt_v2.csv`
- Rows: 1,257 data rows
- Date range: 2019-01-02 through 2023-12-28
- Features: 36 combined market/news features
- SHA-256: `8817744cf1138971613ace2d137bc146c867e5dda4fc79919dca8d356ffc05e9`
- FinBERT model/tokenizer: `finbert-v1` / `finbert-tokenizer-v1`
- FinBERT ONNX SHA-256: `4a8d58ba2f8d74c7fca30fdb49fbbe367b64760104b64e8623e896a007229a6e`
- Vocabulary SHA-256: `07eced375cec144d27c900241f3e339478dec958f92fddbc551f295c992038a3`

The frozen ONNX artifact exposes the three-class sentiment output but no second
embedding output. Consequently, `embedding_0` through `embedding_7` are zero for
all rows in this rebuild. This result is a sentiment-plus-market experiment, not
the final embedding-enhanced architecture.

## Training and evaluation

The fixed chronological split was:

- Train: 2019-01-02 — 2020-12-31, 505 rows
- Validation: 2021-01-04 — 2022-12-30, 503 rows
- Test: 2023-01-03 — 2023-12-28, 249 rows

Default CPU XGBoost produced test accuracy `0.578313`, ROC AUC `0.500496`, and
log loss `0.680929`. The validation-only 72-configuration search improved the
test ROC AUC to `0.540079` and log loss to `0.679620`, but accuracy remained the
majority baseline at `0.578313`; the selected model stopped after one boosting
round and its test probability standard deviation was only `0.012526`.

The three expanding-window directional walk-forward test AUCs were `0.531850`
(2021), `0.493053` (2022), and `0.521362` (2023), for a simple mean of `0.515422`.
The trainer marked the target as not a promotion candidate. No Sharpe is claimed
from this retraining because the trainer output is classification-only and the
candidate was not connected to the cost-aware strategy evaluator.

## Artifacts

- Default candidate: `artifacts/xlk_news_xgboost_rebuilt_v2.json`
- Validation-search candidate: `artifacts/xlk_news_xgboost_rebuilt_v2_hpo.json`
- Walk-forward report: `artifacts/xlk_xgboost_finbert_rebuilt_v2_walk_forward.json`

Both candidates remain research artifacts and are not wired into the active API
model path.

## Evidence-valid evaluation milestone

The C++ trainer now supports the explicit `xlk-logits-only-combined-features-v3`
contract, rejects a full combined dataset when its embedding columns are
zero-variance, writes a feature manifest, and supports purged monthly
walk-forward evaluation with trailing six-month validation and saved OOS
predictions. The existing 36-column source was evaluated after selecting only
its truthful market/logits fields; it was not treated as an embedding dataset.

The 36 monthly OOS folds produced the following fixed, untuned ablation results:

| Profile | Accuracy | Log loss | ROC AUC |
|---|---:|---:|---:|
| Class prior | 0.518617 | 0.699014 | 0.464882 |
| Market-only | 0.511968 | 0.702902 | 0.475953 |
| FinBERT logits-only | 0.525266 | 0.700474 | 0.484959 |
| Market + logits | 0.509309 | 0.703005 | 0.480550 |

These results do not meet promotion criteria. The true pooled FinBERT embedding
experiment remains blocked until an ONNX graph with a validated pooled output is
available.

## Volatility/regime iteration

The directional next-close result was not promoted. Following a separate
pre-registered target review, the experiment was changed to predict whether the
next session's open-to-close absolute return exceeds the median of the preceding
20 XLK open-to-close absolute returns. The prediction is made after session `t`
and evaluated on session `t+1`; monthly folds purge one session before validation
and test.

The fixed XGBoost configuration was depth 2, eta 0.03, minimum child weight 10,
subsample and column sample 0.8, lambda 10, maximum 400 rounds, early stopping
30, and seed 42. The market-volatility block contains the existing nine market
features plus overnight gap, intraday range, absolute open-to-close move,
5/10/20/60-session realized volatility, volume z-score, and SPY realized
volatility. QQQ/VIX and FinBERT-logit additions were tested as development
ablations but were not selected after the untouched check.

Development, 36 purged monthly folds through 2023, using the actual FinBERT-logit
dataset but evaluating the market feature profiles:

| Profile | Accuracy | Log loss | ROC AUC |
|---|---:|---:|---:|
| Market-only | 0.562500 | 0.680547 | 0.589236 |
| Market + volatility | 0.571809 | 0.681058 | 0.589852 |
| Market + volatility + QQQ/VIX | 0.582447 | 0.678099 | 0.602556 |

The QQQ/VIX development lift did not persist. On the untouched 2024–2025
period, the simpler market + volatility candidate produced pooled raw AUCs of
0.582056, 0.577840, 0.586215, 0.582542, and 0.575441 across seeds 42–46
(mean 0.5808). Seed 42 had raw AUC 0.58204, log loss 0.68243, balanced
accuracy 0.53949, and a circular 20-session block-bootstrap 2.5th-percentile
AUC of 0.52308. Market-only seed 42 was lower at AUC 0.56260 and log loss
0.68813. These are research diagnostics, not a promotion claim.

The risk audit did not clear the strategy gates. The fixed probability overlay
had 10-bps Sharpe 0.049, versus 0.077 for exposure-matched buy-and-hold; the
fixed 100%/50% top-quartile overlay had 10-bps Sharpe 0.141 and 20-bps Sharpe
0.014. Its untouched risk quintile mean absolute returns were
`0.00608, 0.00732, 0.00966, 0.01012, 0.00989`, which is not monotonic in the
highest quintile. Fold-local Platt calibration reduced rather than improved
the candidate's untouched log loss and Brier score, so the raw ranking remains
the primary diagnostic and the calibrated output is retained only for audit.

Decision: freeze this as a research-only volatility indicator and do not wire it
into the active recommendation model or claim a trading edge. The active model
artifact remains unchanged. A fixed seed-42 confirmation on the newly available
2026-01 through 2026-07 period produced raw AUC `0.59543`, but its block-
bootstrap lower bound was only `0.50746`, its fixed 10-bps model overlay Sharpe
was `0.882`, and the top-quartile overlay Sharpe was `0.718`; seven monthly folds
are not enough to override the failed 2024–2025 gates. No tuning was performed
on 2026.

The reproducible evaluator is `arrakis-evaluate-volatility`. Its final untouched
report is `artifacts/xlk_volatility_final_evaluation_untouched_2024_2025_seed42.json`.
The market-volatility development report is
`artifacts/xlk_volatility_marketvol_calibrated_dev_2019_2023_seed42.json`, and
the five untouched seed reports are
`artifacts/xlk_volatility_marketvol_untouched_2024_2025_seed42.json` through
`seed46.json`. The temporary 2019–2025 market-only evaluation input has SHA-256
`d1bd770803ac8dfcb86e027ce13197dd0b3f6bd43b94417350bc3e6041187be8`.

The 2026 confirmation report is
`artifacts/xlk_volatility_final_evaluation_untouched_2026_seed42.json`, using
the generated 2019–2026 input with SHA-256
`f7f90739d7229ce6cef7e30373db38b929ff40c44fe6af99598f8a0e80a3077a`.

## Continuous-volatility pivot

The prescribed continuous pivot was also run once on the development period.
It predicts the next-session squared open-to-close return with the same fixed
shallow XGBoost family and evaluates QLIKE, volatility MAE, variance RMSE, and
correlation. Across 752 purged monthly OOS rows through 2023, the model's QLIKE
was `1.35320`, worse than rolling 20-session intraday variance at `1.24286` and
EWMA half-life 20 at `1.22469`. It was therefore not sent to the untouched
period and was not tuned further.

Report: `artifacts/xlk_volatility_regression_dev_2019_2023.json`.

## Final volatility regression check and pooled-sector direction check

The prescribed log-variance regression target was then used without changing the
feature block or adding FinBERT. The target is
`log(open_to_close_return[t+1]^2 + 1e-8)`, with fold-local multiplicative scale
fit on validation data and QLIKE-based early stopping. It failed the pre-registered
gate in every evaluation window:

| Window | Model QLIKE | EWMA QLIKE | Improvement | Favorable folds |
|---|---:|---:|---:|---:|
| 2019–2023 development | 1.21595 | 1.21032 | -0.00563 | 18/36 |
| 2024–2025 untouched | 1.79957 | 1.69024 | -0.10934 | 14/24 |
| 2026-01–2026-07 | 1.46048 | 1.38603 | -0.07445 | 4/7 |

The corresponding 20-session paired moving-block bootstrap lower bounds for
model improvement were `-0.07912`, `-0.33023`, and `-0.24678`. No volatility
regression promotion occurred.

A pooled 11-sector ETF panel was also built with a full-session purge. Its fixed
market/context/cross-sectional next-session direction model produced development
AUC `0.50333` and log loss `0.69358` across 48 monthly folds. This is not a
recommendation result and was rejected.

The frozen FinBERT ONNX graph originally exposed logits only. The local graph
contains the final BERT hidden state, so `model_with_pooled_embedding.onnx` was
derived with a named `pooled_embedding` output using attention-mask mean pooling;
the C++ smoke tests validate a nonzero 768-dimensional output. The builder was
also changed to require that output, deduplicate by the normalized article hash,
and preserve point-in-time cutoffs. The first implementation was replaced by the
resumable C++ cache builder `arrakis-build-finbert-embedding-cache`, which stores
the content-hash index, logits, pooled vectors, model/tokenizer checksums, and
checkpoint manifest after every batch. Its fixed 1,000-headline benchmark over
the 2019–2023 XLK source completed 1,000/1,000 rows with zero failures in
142.071 seconds (batch 64, max 64 tokens, 8 ONNX Runtime threads). The full
eligible set contains 67,313 unique articles across 1,548 dates, which projects
to approximately 2.66 hours at the measured rate; runtime is therefore no
longer the primary blocker. Coverage is: one sector, because the repository
contains only XLK historical membership and XLK-normalized news. This fails the
pre-registered event-conditioned panel minimum of three active sectors per date,
3,000 rows, and all 11 configured sector ETFs. The cache benchmark is recorded
at `/private/tmp/finbert_cache_benchmark/manifest.json`; it is a local benchmark
artifact rather than a promoted model dataset.

Because the required cross-sector coverage is unavailable, no embedding-enhanced
metric was reported and no full cache or XGBoost panel training was run. The
logits-only artifact remains ineligible for the final FinBERT-plus-XGBoost
promotion gate. A valid next iteration requires independently sourced,
point-in-time historical membership and news coverage for at least three sectors;
the current repository cannot produce that evidence without adding data.

Decision: retain the existing XGBoost model only as an auditable research
baseline, fail closed to Neutral for production-style recommendations, and make
no profitability or predictive-edge claim.

## Corrected multi-sector session assignment and control iteration (2026-08-11)

The earlier coverage conclusion is superseded by the generalized SEC N-PORT
import and the availability-safe multi-sector FNSPID import. The holdings table
now covers XLB, XLC, XLE, XLF, XLI, XLK, XLP, XLRE, XLU, XLV, and XLY. SEC
quarterly snapshots are usable only strictly after their filing
`available_from` date; same-day articles are excluded. The holdings CSV SHA-256
is `be48d82e8a7e414752d0bd4cb5b17d6cb3961e9931d13917cd71c2b4161a0933`.

FNSPID's released timezone normalization could not be independently trusted, so
the corrected importer marks every retained timestamp as
`fnspid_utc_unverified`, preserves the raw source value, and assigns each row to
the first SPY session strictly after its source calendar date. This is
deliberately conservative: source clock time is not used for same-session
eligibility. The importer also selects one canonical row per sector/content hash,
with the earliest assigned session winning, and sorts the output
deterministically.

The corrected normalized artifact contains 260,560 rows across 11 sectors and
1,026 sessions. It has 964 sessions with at least three active sectors, 950 with
at least eight, and a maximum of 11 active sectors. The normalized CSV SHA-256 is
`3141253f5101b1dea0e2b887c48a49249ad7246b26cafbdfb121114eb6bdad2c`. The import
manifest is `data/fnspid/manifests/sector_import.json`; the model-free coverage
audit is `data/fnspid/manifests/sector_coverage_audit.json`. The audit reports
178 content hashes spanning multiple sessions across sectors; this remains an
explicit diagnostic and is not silently treated as additional signal.
The deterministic conflict list is
`data/fnspid/manifests/sector_coverage_audit.json.conflicts.csv`.

The market-only pooled control was rerun with 67 monthly folds and an 11-row
full-session purge. It produced pooled AUC `0.499127` and log loss `0.693371`.
A corrected news-coverage control joined session-level article count, coverage,
unique-content ratio, and publisher breadth to the same pooled market panel. It
produced 36 monthly folds, pooled AUC `0.496286`, and log loss `0.693762`, worse
than the market-only control. Neither result is a recommendation or promotion
candidate.

FinBERT throughput remains below the preregistered gate. The exact-token-input
benchmark completed 1,000 of 1,000 inputs in 163.563 seconds at approximately
6.1 unique inputs/second, below the 12.3 inputs/second go threshold. Larger
batch sizes, graph optimization, CoreML CPU/GPU, ANE, and multi-process sharding
were also slower in the local probes. A batch-512 run improved the bounded probe
to 146.356 seconds (6.8 inputs/second); ORT parallel execution was 159.866
seconds. These remain below the gate. No embedding-enhanced model was trained
from the corrected dataset, and no accuracy or Sharpe claim is made.

As a prespecified target check, a five-session sector excess-return panel was
evaluated with a 55-row purge (five complete 11-sector sessions). The positive
excess target produced AUC `0.496521`. A top-cross-sectional-quartile target
produced AUC `0.523972` under the fixed depth-2, minimum-child-weight-10,
lambda-10 configuration; the bottom-quartile counterpart was lower at `0.509461`.
Neither cleared the per-fold AUC gate.

Decision: the project has improved data validity and cross-sector coverage, but
the corrected controls and inference throughput gates still fail. Keep the
recommendation surface fail-closed to Neutral and retain XGBoost only as an
auditable research baseline until a future verified timestamp source or a faster
equivalent FinBERT inference path produces new untouched evidence.

## Session-grouped pairwise ranking ablation (2026-08-11)

Because the five-session target is a cross-sectional top-sector label, GPT-5.6-sol
recommended one objective-alignment ablation: XGBoost `rank:pairwise` with each
prediction session as a query group. The existing `>= 0.7` normalized rank label
means the positive class is the top four of eleven sectors, not the top three; the
existing binary result is therefore retained as the comparable top-four baseline.

The C++ ranker requires exactly 11 contiguous ETF rows per session, excludes
`sector_id`, attaches the 11-row group vector to train and validation matrices,
uses the same 55-row/five-session purge, and evaluates the same 67 monthly folds.
Its one frozen configuration is XGBoost 3.3.0, `rank:pairwise`, `auc`, depth 2,
eta 0.03, minimum child weight 10, subsample 0.8, column subsample 0.8,
lambda 10, seed 42, `lambdarank_pair_method=topk`, 11 pairs per sample,
normalization enabled, 400 maximum rounds, and 30-round validation early stopping.

The ranker completed all 67 folds but produced pooled test AUC `0.513372`. It
failed the promotion rule: validation AUC was below `0.55` in most folds and
multiple test folds were also below `0.55`. No ranker artifact was promoted and
no probabilities or trading claims were derived from its raw ranking scores.
The reproducible report is `/private/tmp/pooled_sector_5session_top_ranker.json`;
the implementation is `services/ml_model/src/train_xgboost_ranker.cpp`.

Decision: close the ranking-objective branch after this pre-registered ablation.
Further model/objective searches on the same folds would increase data-snooping
risk without addressing the dominant verified-timestamp and FinBERT-throughput
limitations. A future promotion attempt requires new point-in-time information or
an untouched confirmation period.

## Inverse FNSPID timestamp candidate check (2026-08-11)

To test whether the released FNSPID timestamp transform was merely inverted by
the conservative policy, an isolated candidate import was run. Non-midnight raw
timestamps were repaired with the inverse EST/EDT offset and admitted only when
the repaired time was no later than the 09:20 ET pre-open cutoff; date-only rows
remained conservatively delayed. This is a hypothesis check, not a replacement
for verified source timestamps.

The full 22 GB raw scan completed with 260,560 rows written, 2,161,624 rows
skipped, and 47,520 duplicates. Only 1,929 rows had usable non-midnight clock
values; 258,631 remained date-only. The candidate coverage audit found 964 of
1,026 sessions with at least three active sectors, 950 with at least eight, and
214 content hashes spanning multiple sessions. The candidate CSV SHA-256 is
`73299d91d2d5bdb61d44d76dcd88abf30497a59ee2074b0d4dfad91d6c35f997`.

Using the same fixed news-control features, model, monthly folds, and 11-row
session purge, the candidate produced 36-fold pooled AUC `0.498478` and log
loss `0.693644`, compared with conservative-policy AUC `0.496286` and log loss
`0.693762`. The small AUC movement is not a meaningful improvement, does not
clear the `0.55` promotion gate, and does not justify the expensive FinBERT
embedding pass. No candidate model was promoted.

Decision: retain the conservative normalized artifact as authoritative, retain
the inverse candidate only as a reproducible audit artifact, and stop timestamp
policy/model hunting on this unverified corpus. Any further news-model work
requires independently verified publication timestamps or a new point-in-time
dataset; otherwise the recommendation surface remains Neutral.

## Official SEC filing text and frozen FinBERT sentiment check (2026-08-11)

To replace the unverified article-time source with an authoritative point-in-time
source, official SEC submissions JSON was fetched for the earliest 2019 top-five
holdings snapshot across all eleven sector ETFs. The C++ importer retained only
8-K, 8-K/A, 10-Q, 10-Q/A, 10-K, 10-K/A, 6-K, 20-F, and 40-F records with an exact
`acceptanceDateTime`, required point-in-time membership, and assigned each filing
to the first SPY session strictly after its acceptance calendar date. The event
artifact contains 3,467 rows from 2019-01-01 through 2023-12-31, with 0 missing
submissions; the event CSV SHA-256 is
`9489d4ccf77a1d8c62341f78dace8e375844f2c31cd8b866c98a97921c3418cd`.

All 2,722 retained 8-K primary documents were downloaded from SEC Archives with
zero failed URLs. The frozen `finbert-v1` logits-only ONNX model and tokenizer
were run locally with 64-token inputs. The initial visible-HTML pass had 2,691
unique inputs and produced a 36-fold pooled AUC of `0.510762`; it was rejected
after an input audit showed that hidden XBRL/header and cover content dominated
the first tokens. The final bounded extraction pass omitted hidden XBRL,
`display:none`, scripts/styles, and comments, retained visible Item sections
except Item 9.01, and truncated each filing at 8,192 characters. It produced
2,113 unique model inputs in 184.900 seconds. The sentiment CSV SHA-256 is
`1e98ff4268d28fa49ad3f400bab009f254df432e21eed8d47971541563f85952`.

The final sentiment feature was the mean `positive_probability -
negative_probability` per sector/session, joined to the existing SEC metadata
control. The target, 55-row purge, 36 monthly folds, fixed depth-2 XGBoost
configuration, seed, and early-stopping protocol were unchanged. Results were:

| Dataset | Pooled AUC | Log loss | Promotion result |
|---|---:|---:|---|
| Market-only development control | 0.513307 | — | rejected |
| SEC metadata control | 0.514197 | 0.655277 | rejected |
| SEC metadata + initial FinBERT | 0.510762 | 0.655317 | rejected |
| SEC metadata + Item-section FinBERT | 0.510150 | 0.655360 | rejected |

The final Item-section run completed all 36 folds; only 2 folds cleared both
validation and test AUC `>0.55`. Validation AUC ranged from `0.450279` to
`0.571358`; test AUC ranged from `0.382568` to `0.672023`. It therefore fails
the established per-fold gate and does not support a predictive-edge or Sharpe
claim. The final pooled dataset and walk-forward report were written to
`/private/tmp/pooled_sector_5session_sec_finbert_items_control.csv` and
`/private/tmp/pooled_sector_5session_sec_finbert_items_control_monthly.json`.

Decision: close the SEC-sentiment branch after the one permitted semantic-text
iteration. Do not search alternative chunking, exhibit selection, aggregation,
objectives, or hyperparameters on these development folds. Keep the research
surface fail-closed to Neutral; any future promotion attempt requires a new
verified point-in-time dataset or an untouched confirmation period.

## Corrected pooled next-close target and verified fundamentals check (2026-08-11)

GPT-5.6-sol audited the pooled-sector builder and found that its column named
`target_next_close_up` was not the final recommendation target: the default label
was a sector future open-to-close excess return relative to SPY. The top/bottom
variants were cross-sectional rank labels. The builder now supports an explicit
`close-direction` mode whose label is exactly
`close[sector,t+1] > close[sector,t]`; all feature values remain calculated only
through close `t`. The original relative-return and ranking modes remain
available for legacy research artifacts.

The corrected panel contains complete 11-sector sessions and was evaluated with
one complete 11-row session purge, fixed depth-2 XGBoost, eta `0.03`, minimum
child weight `10`, subsample/column sample `0.8`, lambda `10`, 400 rounds,
30-round validation early stopping, and seed `42`. On the same 36 development
folds from 2021-01 through 2023-12, the true close-direction target produced:

| Dataset | Pooled AUC | Validation/test folds both >0.55 |
|---|---:|---:|
| Corrected market/context panel | 0.516061 | 3/36 |
| Corrected panel + SEC companyfacts features | 0.512950 | 4/36 |

The corrected market panel was built from the local sector ETF histories; its
CSV SHA-256 is
`06434c101710eceaf94d9ccb17a9bad4625bcd4567d613ddf2d11315618d7edb`. The
fundamentals join loaded 54 official SEC companyfacts files and used only facts
whose accession matched an accepted 10-Q/10-K event. It added filing count,
issuer coverage, revenue growth, net margin, asset growth, liability growth, and
EPS change. The joined CSV SHA-256 is
`93da244e82be80567f976f33ed458b47797e4472d8d87e9af6099ef810ddde95`.

As a sample-length audit, a deterministic XLK market-only panel from
2016-10-25 through 2026-07-24 was also evaluated. Its 103-fold pooled direction
AUCs were `0.496472` for market features, `0.494743` after OHLCV volatility
features, and `0.491855` after QQQ/VIX context features. Expanding the history
does not rescue the direction result.

Decision: the pooled target-definition defect is fixed and the first verified
fundamental-information iteration is rejected. Do not promote the old pooled
artifacts or interpret their prior AUCs as next-close direction evidence. Keep
the recommendation surface Neutral; a further promotion attempt requires a new
verified source with direct directional information, such as point-in-time
expectation-relative earnings surprises or options-implied features, rather
than additional OHLCV/model search.

## SEC insider-event information check (2026-08-11)

As a second verified SEC information check, official submissions JSON was used to
aggregate Form 4/4-A, Form 3/3-A/5/5-A, and Schedule 13 filings by sector/session.
The assignment used each filing's exact `acceptanceDateTime` and the first market
session strictly after its acceptance calendar date. The resulting artifact
contains 4,648 rows with nonzero insider or ownership activity and has SHA-256
`e12fd2d87bd4451864e5d90f36bb9cda46ecc9255e423ed9261a0c047215bb5c`.

On the corrected close-direction target and the same 36 development folds, the
fixed XGBoost run produced pooled AUC `0.505652`; only 3/36 folds passed both
validation and test AUC `>0.55`. This feature is a filing-activity count only;
no transaction-level buy/sell direction was inferred. It is rejected and not
promoted.

Decision: close the local market/SEC feature-search branch. The evidence now
supports a data-source substitution, not more architectures, hyperparameters,
OHLCV features, SEC filing counts, or SEC fundamentals. The unresolved
requirement is licensed point-in-time textual news with version history and
entity identifiers; until that source is available and passes the fixed gate,
the recommendation surface remains fail-closed to Neutral.

## Version-aware MRN ingestion contract (2026-08-11)

The next data-source iteration is now implemented as the C++20 executable
`arrakis-build-mrn-sector-articles`. It accepts JSONL records following the
licensed LSEG Machine Readable News shape (`id`, `altId`, `firstCreated`,
`versionCreated`, `takeSequence`, `messageType`, `pubStatus`, `provider`,
`headline`, `body`, and `subjects`) plus an entity map, point-in-time sector
holdings history, and the local SPY market calendar. The adapter is deliberately
source-contract-only: no licensed MRN archive or credentials were present in
the workspace, so it has not been used to claim model performance.

The adapter sorts versions by `versionCreated` and `takeSequence`, applies a
16:00 America/New_York cutoff without using the host timezone, maps after-close
versions to the next market session, and keeps corrections confined to their
assigned session. Withdrawal/delete/retract messages remove the current
session candidate, including when the withdrawal has no repeated subject list.
Entity mapping is joined to holdings only when `available_from` is strictly
before the assigned trading date and the effective membership interval contains
that date. Every output row includes normalized UTC timestamps, version and
take identifiers, a content hash, and a manifest with rejected records,
unknown entities, membership rejects, withdrawals, and the cutoff policy.

The deterministic fixture
`backend/tests/fixtures/mrn_small.jsonl` exercises a pre-close version, an
after-close correction, a same-session correction, an unknown entity, and a
withdrawal without subjects. It produced two rows: the original story on
2019-01-02 and the corrected story on 2019-01-03; the withdrawn candidate was
absent. The next promotion attempt should run this adapter on a licensed,
point-in-time archive, then reuse the frozen FinBERT and fixed XGBoost
walk-forward configuration without further local model search.

## Final local SEC alignment check and negative ablations (2026-08-11)

One additional market-only feature family was tested as an opt-in
`rich-direction` build: sector intraday return, overnight gap, cross-sectional
mean/dispersion/breadth at 1/3/6 sessions, and SPY/QQQ intraday and gap values.
The fixed XGBoost configuration and 36 purged monthly windows were unchanged.
With the required 11-row full-session purge, the rich panel produced pooled AUC
`0.505954`, versus `0.516061` for the corrected close-direction baseline. It
is rejected; no parameter search was performed around it. The rich dataset SHA-256
is `6242542aff8c3a86f15a65a92b47b7a7191659b62cd5d74daff56d7c1a218e9e`.

GPT-5.6-sol then identified one concrete timing correction in the official SEC
branch. The original importer assigned every filing to the first SPY session
strictly after its acceptance calendar date, which delays filings accepted before
the next open. The importer now supports `preopen-next-close`: it retains only
filings accepted in `(prior session 16:00 ET, event session 09:20 ET]`, checks
point-in-time membership on the event session, and joins the article to the prior
session feature row whose label is `close[t+1] > close[t]`.

The preregistered alignment run retained 2,825 SEC events, 2,371 visible-text
8-K inputs, 1,865 unique FinBERT inputs, and 2,041 sector/session embedding
aggregates. It used the frozen pooled-embedding ONNX graph, all 768 embedding
dimensions, the fixed XGBoost configuration, and an 11-row purge across 36
monthly folds. Results were:

| Dataset | Pooled AUC | Validation minimum | Test minimum | Both gates | Decision |
|---|---:|---:|---:|---:|---|
| Corrected market baseline | 0.516061 | 0.470214 | 0.371243 | 3/36 | rejected |
| Rich OHLCV ablation | 0.505954 | 0.447870 | 0.358705 | 5/36 | rejected |
| SEC pre-open + full FinBERT embeddings | 0.517393 | 0.470342 | 0.345238 | 3/36 | rejected |

The pre-open event CSV SHA-256 is
`b6fbe5ee3c17010fba4ee0719acf260cda116ab50612f93d19236c8832144e20`, and the
final pooled dataset SHA-256 is
`8acc48830d0112b006714e3271870a62cde35707e8c16c08942fcd2aae2d8b01`.
The SEC branch is now closed after the single timing correction, as recommended
by GPT-5.6-sol. No local directional model clears the established gate.

A historical Finnhub `company-news` probe using the existing local credential
returned an empty result for the 2019–2023 request, so it cannot supply the
missing verified archive. The version-aware MRN adapter remains ready for a
licensed archive; until one is available, the recommendation surface remains
fail-closed to Neutral and no Sharpe or predictive-edge claim is made.

## Linked SEC Exhibit 99.x follow-up (2026-08-11)

Following GPT-5.6-sol's distinction between post-hoc Item 2.02 filtering and a
genuinely new source, the repository now contains a C++20 linked-exhibit
manifest builder and exhibit article normalizer. The builder follows only
explicit `href` links from the accepted primary 8-K HTML; it does not guess
attachment filenames. The pre-open event set produced 759 linked Exhibit 99.x
URLs from 2,371 primary documents. All 759 downloads succeeded, and all 759
normalized rows contained visible text after removing SEC wrapper markup,
hidden XBRL/header content, scripts, styles, and comments. The normalized
article SHA-256 is `b37f127471668ce4d28d2998b72117c25ff4fc8d254ed32a4a40bc88174d1ea4`.

The linked exhibits were scored with the frozen FinBERT pooled-embedding graph
(702 unique token inputs, 768 dimensions), assigned to the prior-session
feature row using the same exact acceptance cutoff and 11-row purge, and
evaluated with the unchanged XGBoost monthly walk-forward configuration. The
branch produced pooled AUC `0.500298`, validation minimum `0.460623`, test
minimum `0.348970`, and only `4/36` folds passing both gates. It is rejected;
the result is not used for tuning or promotion. The exhibit manifest SHA-256 is
`5bba3d8c1c731546da91815129ca0388f94c2c660bca649fba2848f8127c3d29`, the
pooled dataset SHA-256 is
`c7b386a39ded592a905b685535b014fbd09c75abb743ecab1c972e4f32872a35`, and the
walk-forward artifact SHA-256 is
`bb409418a69c7f19afc35fede8f8bbf13775bf90a21d81efc68316617cf9db22`.

This is a valid negative source experiment, not evidence that the SEC source
is predictive. The only remaining data-source iteration with a materially
different information set is the licensed, version-aware MRN archive; the
local recommendation surface remains fail-closed to Neutral until that archive
is available and a complete out-of-sample model clears every promotion gate.

## Later untouched market holdout and per-ETF architecture check (2026-08-11)

The local market panel extends through `2026-07-24`, beyond the 2018--2023
period used by the earlier development experiments. Before making any further
model changes, the frozen pooled market panel was evaluated once on the later
holdout from `2024-01` through `2026-07`, with the same 11-session purge,
six-month validation window, fixed XGBoost parameters, and 31 monthly folds.
It produced pooled AUC `0.500447`, validation minimum `0.475534`, test minimum
`0.440643`, and `0/31` folds passing both gates. The market input SHA-256 is
`0e386926295572430a022d05cc751dcd6bfe8f485c530ad7388ac7ce4afe9038`, and the
walk-forward artifact SHA-256 is
`168c03bba96b8ea6e489e8bd888ba113ed4ef0f8e9008282bd6b13c72351796e`.

As a separate architecture check, one frozen classifier per ETF was evaluated
on that same untouched period without changing features, parameters, seed, or
purge. Pooled per-ETF AUCs were:

| ETF | AUC | ETF | AUC |
|---|---:|---|---:|
| XLB | 0.474377 | XLC | 0.466643 |
| XLE | 0.517144 | XLF | 0.456004 |
| XLI | 0.485947 | XLK | 0.484767 |
| XLP | 0.516730 | XLRE | 0.506876 |
| XLU | 0.522978 | XLV | 0.545119 |
| XLY | 0.463130 | | |

The mean per-ETF AUC was `0.494520`; no ETF cleared the `0.55` pooled-AUC
threshold, and no ETF satisfied the every-fold promotion rule. These results
are exploratory evidence only because the later holdout has now informed
research decisions. The precise conclusion is limited to the tested target,
features, XGBoost family, and protocol: no reproducible next-close directional
edge was detected in the local market/context panel. It does not establish
that no future data source or model can ever contain an edge.

Further tuning, TCN trials, horizon changes, feature additions, or selecting a
weakly positive ETF on this same 2024--2026 period would be data-snooped. The
next promotable attempt requires materially new point-in-time news data and a
sealed evaluation period beginning after July 2026, preferably with at least
12 months of observations. Until then, no directional model is promoted.

## Public Yahoo article archive follow-up (2026-08-11)

To test a materially different information source, the local pipeline exported
the public `bwzheng2010/yahoo-finance-data` `stock_news.parquet` archive and
adapted it through the new C++20 `arrakis-build-yahoo-sector-articles` target.
The usable source range was `2025-03-11` through `2026-07-24`: 302,155 source
records became 313,393 point-in-time sector occurrences across 343 sessions and
all 11 ETFs. The source has date-only publication values, so the adapter uses
the first market session strictly after each report date. It does not infer an
intraday cutoff, so this experiment is exploratory rather than a production
quality news-timestamp claim.

The extended SEC N-PORT membership file includes the 2026 Q1/Q2 snapshots and
has SHA-256 `b807988695d8c49a301775c4d0aa2a92247d3e91274e689cccadcb9e23dd5583`.
The raw Yahoo export SHA-256 is
`52f6f91612872512625081acb194a3f0a1b0f23d48d3ec2059ef02f76a66c787`; the
normalized article CSV SHA-256 is
`857a966d0eac4254381d5a1aa64e4b96705120808318247148c4ec1fb0bd6242`.

The frozen FinBERT run processed 207,704 unique token inputs and all 313,393
occurrences with zero failures. The embedding matrix contains exactly
`207704 * 768` float32 values (`638066688` bytes); no occurrence was missing an
embedding. The cache used model SHA-256
`c7f8304257b2a587d9d9b348410b3809cc9403da909cc0331a63294426e4205a`, tokenizer
SHA-256 `07eced375cec144d27c900241f3e339dec958f92fddbc551f295c992038a3`, the
CoreML GPU/static-input provider on the local Apple M5, batch size 64, and a
fixed 64-token input policy. The cache manifest SHA-256 is
`f4df96974049cf315b0d55ac2303dc38a905486643606ac310c8585579a92bb0`.

The pooled panel covered 3,690 sector/session rows with nonzero news
aggregates. The pooled, no-search XGBoost evaluation used 16 monthly folds,
purge 11, and the same fixed feature/model configuration as prior experiments:

| Dataset | Pooled AUC | Validation minimum | Test minimum | Both gates | Decision |
|---|---:|---:|---:|---:|---|
| Yahoo date-only news + market/context | 0.504521 | 0.478037 | 0.450178 | 0/16 | rejected |

Pooled accuracy was `0.507713`; pooled log loss was `0.693105`. Fold-local
calibration reduced pooled AUC to `0.471441` and increased log loss to
`0.695676`, so calibration was not retained. The pooled feature CSV SHA-256 is
`7a5431c37402b5c91e5381bb8f6bcd39a86d39443aef99385dde5ab8332797c8`, and the
walk-forward JSON SHA-256 is
`9678c263cc4f4970ef59145be8a9073f02cc6540bc29dd9597c34ee9cc83abaa`.

The required one-classifier-per-ETF check on the same frozen period also did
not clear the every-fold gate:

| ETF | Pooled AUC | Validation minimum | Test minimum | Passing folds |
|---|---:|---:|---:|---:|
| XLB | 0.478889 | 0.494613 | 0.255102 | 2/16 |
| XLC | 0.471845 | 0.420281 | 0.305556 | 5/16 |
| XLE | 0.519075 | 0.466824 | 0.211538 | 1/16 |
| XLF | 0.464891 | 0.430764 | 0.200000 | 3/16 |
| XLI | 0.572415 | 0.441745 | 0.311224 | 5/16 |
| XLK | 0.486380 | 0.446815 | 0.240741 | 0/16 |
| XLP | 0.518279 | 0.440260 | 0.398148 | 4/16 |
| XLRE | 0.506125 | 0.469711 | 0.273810 | 2/16 |
| XLU | 0.563031 | 0.429025 | 0.313636 | 6/16 |
| XLV | 0.546602 | 0.471631 | 0.350000 | 2/16 |
| XLY | 0.459486 | 0.413136 | 0.218182 | 0/16 |

The XLI and XLU pooled AUCs are isolated positive aggregates, not promotion
evidence: their validation/test minima are far below the threshold and neither
passes all folds. The Yahoo branch is rejected without tuning, horizon changes,
threshold changes, or selecting a favorable ETF. The recommendation surface
remains fail-closed Neutral. A future promotable attempt requires a source with
verified publication timestamps and a sealed evaluation period after July 2026;
this two-year date-only archive is not sufficient for that claim.

## Timestamped Yahoo archive development generation (2026-08-11)

The next source audit used `luckycat37/financial-news-dataset`, a public
research corpus documented as 51,272 Yahoo Finance articles from 2017--2023.
Its license is CC-BY-NC-SA-4.0, so the corpus and any derived artifacts remain
quarantined for non-commercial research pending a separate provenance review.
The adapter is the C++20 `arrakis-build-luckycat-sector-articles` target. It
reads only `date_publish`, `date_download`, title/body text, URL, publisher,
and `mentioned_companies`; it physically ignores the source's precomputed
prices, sentiment, and emotion fields.

For the XLK generation, the source was restricted to 2019--2023 because
verified point-in-time SEC membership starts in 2019. Availability is the
later of `date_publish` and `date_download`, interpreted as UTC. Availability
at or before the 09:20 ET pre-open is assigned to that session; later
availability is assigned to the next exchange session. Membership requires
`available_from` to be strictly before the assigned session. Exact duplicate
content is collapsed per sector and assigned session. The normalized output
contains 22,116 XLK occurrences over 780 sessions, 22,099 content hashes, and
22,001 unique FinBERT inputs. The normalized article SHA-256 is
`3f58a0fd34e3ad8f68d6086ded5fa2ea9f0ae5f253dd9daf3d7d3a02da95e0ab` and its
import manifest SHA-256 is
`6348dc5e3e30a26f8b9105ae6a05ecc730a29f35f1093c153019a78c6cfa7b3b`.

The frozen FinBERT cache completed on the local CoreML GPU/static-input path
with zero failures. Its current cache contains 26,083 deterministic rows
(22,001 are required by this XLK occurrence set); the occurrence CSV SHA-256
is `624f4a178def5d3630d77dedf181a2e1d6ba7f09d82292a2c5f5cb4552acbfcf`, the
index SHA-256 is `fda853cf0a17064d34c5e3c79b0ac1e6ea90dec1ac1d659c5073ee7e4fcd20e2`,
the embedding matrix SHA-256 is
`ef25a36bcb7812fe49532841c4daa55e79f1420a5a0af0005eecc68e5234c4ba`, and the
cache manifest SHA-256 is
`3a34207a9df4328a867e5806398bd5aad69080c97a8e27cec9fd0b054663d38d`.
The frozen model and vocabulary hashes remain
`c7f8304257b2a587d9d9b348410b3809cc9403da909cc0331a63294426e4205a` and
`07eced375cec144d27c900241f3e339dec958f92fddbc551f295c992038a3`.

An architecture-correct XLK-only comparison used 33 successive monthly
outer-test folds from 2021-04 through 2023-12, a trailing six-month
validation window, 11 purged sessions, fixed XGBoost settings, and no search:

| Dataset | Pooled accuracy | Pooled log loss | Pooled AUC | Validation minimum | Test minimum | Both-gate folds | Decision |
|---|---:|---:|---:|---:|---:|---:|---|
| XLK market-only control | 0.517341 | 0.699559 | 0.496991 | 0.409748 | 0.240741 | 3/33 | rejected |
| XLK market + frozen FinBERT embeddings | 0.536127 | 0.695306 | 0.518676 | 0.451129 | 0.205357 | 3/33 | rejected |

Embeddings improved the pooled AUC by `+0.021685` and reduced log loss by
`0.004253` relative to the XLK control, but the result is far below the
required `>0.55` validation and test AUC in every fold. Fold-local calibration
further reduced AUC to `0.504311` and increased log loss to `0.700931`, so it
was not retained. The market-only walk-forward SHA-256 is
`a716261bbd3405e46f2d0c2186b61551309772abede4eda4bd1b7978b8d7fb8c`; the
embedding walk-forward SHA-256 is
`8c53f5e5254b6940ce9603f74aec9ee3c6e9501d35185ced826f2457fd0448a1`.

This is a useful implementation result—the timestamped, point-in-time,
full-text FinBERT path is reproducible and provides an incremental signal in
this development window—but it is not predictive evidence sufficient for
promotion. This development generation is frozen and rejected. The local
recommendation surface remains fail-closed Neutral. Further tuning on these
labels would be data-snooping; the next legitimate promotion attempt needs a
licensed, source-verified archive and a sealed post-July-2026 evaluation
period, preferably at least 12 months.

## Full FinBERT aggregation diagnostic (2026-08-11)

The prior embedding comparison did not include the frozen FinBERT probability and sentiment
aggregates. The C++ combined-dataset builder now accepts
`ARRAKIS_FINBERT_INFERENCE_BATCH_SIZE` (default `64`) so local GPU inference can be run in bounded
requests; the previous hard-coded 256-row request did not produce a durable artifact for the
larger archive. A durable 2019--2020 diagnostic was generated with the pooled-embedding graph,
then trained using only the 19 market-plus-FinBERT-logit features (`--feature-subset logits-only`).

The 504-row dataset SHA-256 is
`0ffb71a5949d188e1297c52d998fa571036f80faabdc68aa2da20831f6fc822a`, and its manifest SHA-256 is
`66bfa229b9481b07db174850bf7b85afb2c346ef50fde29444a164997aed5c79`. A chronological diagnostic
split trained through 2020-06-30, validated through 2020-09-30, and held out 2020-10-01 through
2020-12-30:

| Split | Accuracy | Log loss | ROC AUC |
|---|---:|---:|---:|
| Validation (64 rows) | 0.6406 | 0.6600 | 0.4613 |
| Held-out (63 rows) | 0.5397 | 0.6947 | 0.4954 |

The held-out probabilities were nearly constant (mean `0.5866`, standard deviation `0.0073`),
so adding sentiment aggregates did not create a credible directional edge in this diagnostic.
It is not a promotion candidate: it lacks the required later evaluation period and fails the AUC
threshold. The sentiment-only full 2019--2023 generation was stopped after a bounded runtime
without using partial state; no result was recorded from it.

## Corrected snapshot membership and FinBERT probability iteration (2026-08-11)

The first Luckycat sector generation contained a point-in-time membership defect. Its resolver
selected the latest row independently for each symbol. Because the SEC holdings source uses
`effective_to=2099-12-31` as a sentinel, symbols that left an ETF could remain eligible forever.
Those Luckycat metrics are superseded and must not be used. Both the Luckycat and MRN adapters now
select the complete holdings snapshot keyed by the latest `available_from` strictly before the
assigned trading session. The MRN fixture now covers a symbol removed in a later snapshot and
passes the rejection assertion.

The corrected all-sector Luckycat output covers 2019--2023, with 65,026 occurrences, 788 eligible
sessions, eight sectors with source coverage, and 53,299 distinct content hashes. Its CSV SHA-256
is `1d7390d0af30dcb4c0a3204d2a20dbde03cfbca0c9d05fe58cbe0111b3546bc7`; its manifest SHA-256 is
`d561bed7ee2761ab93004a0c713476c0c2e05782221e7747e621d7f1d60a9708`. The source remains the
CC-BY-NC-SA research archive described above; this is not commercial-data licensing evidence.

The frozen FinBERT cache was rebuilt with the pooled-embedding graph and now stores 768 pooled
dimensions plus four probabilities/features per token-input hash:
`positive_probability`, `negative_probability`, `neutral_probability`, and `sentiment_score`.
It completed 53,101 unique model inputs with zero failures. The cache manifest SHA-256 is
`3d050e070a54e15f46c8afd1183c9380609d849cd98bffcb5bded0c9180d4588`; the article-occurrence
CSV SHA-256 is `9a061c1d0c6cd670f1243b328af8a5fee87c019eb97d8736c927a78200374482`. Inference used
the local CoreML GPU/static provider; ONNX Runtime reported that shape-bookkeeping nodes were
assigned to CPU, so this run is not an all-CPU run and the provider limitation is recorded here.

The new aggregator produced 20,900 market rows and covered 3,967 sector/session news aggregates.
The feature panel manifest SHA-256 is
`c1dcd15a11375bac3a84211c7afbd3146a4f610234a27cfb943a167f917a6334`.

The fixed 33-fold protocol is unchanged: monthly outer tests from 2021-04 through 2023-12,
11-session purge, trailing validation, frozen feature/model versions, and no test-based selection.
The validation-only sweep selected the default XGBoost settings; alternatives B/C/D had lower
validation means. Results are:

| Corrected panel | Pooled test AUC | Validation mean / minimum | Test mean / minimum | Both gates |
|---|---:|---:|---:|---:|
| Market + embeddings + sentiment, sector ID retained | 0.519030 | 0.529105 / 0.482035 | 0.514615 / 0.360882 | 1/33 |
| Market + embeddings + sentiment, sector ID excluded | 0.528856 | 0.527483 / 0.487356 | 0.526770 / 0.359492 | 2/33 |
| XLK-only, market + embeddings + sentiment | 0.544877 | 0.550409 / 0.474801 | 0.553033 / 0.338462 | 7/33 |

FinBERT probability features improved the pooled all-sector result versus the corrected embedding-
only no-sector-ID control (AUC `0.515033` to `0.528856`), but no candidate clears the required
`>0.55` validation and test AUC in every fold. The candidate remains rejected and the live
recommendation surface must remain fail-closed Neutral. No Sharpe, profitability, or predictive-edge
claim is made from this generation.

The selected no-sector-ID panel SHA-256 is
`6639567f444660b53ac7cc27257a021127361f0fb4668d8e5877d3777452f8a4`; its walk-forward report
SHA-256 is `49ea968108ce17a9aec16a1d83776394598422cf88b00931e40386f34210ec`. The validation-only
regularization alternatives B/C/D produced validation means `0.515556`, `0.514365`, and `0.522568`
versus `0.527483` for the default. Reduced 32- and 128-dimension embedding panels also underperformed
on validation (`0.525785` and `0.517856`), so the full 768-dimension representation was retained for
the rejected research artifact. These comparisons do not turn a failed experiment into promotion
evidence.

## Follow-up architecture experiments (2026-08-11)

The repository's `rich-direction` market panel was regenerated and joined to the same corrected
FinBERT cache. It adds sector intraday return, overnight gap, cross-sectional mean/dispersion/
breadth, and SPY/QQQ intraday/gap features using date-`t` data only. The rich market CSV SHA-256 is
`9a1926b77158275bc18d31c3a6e4443d189eb10d2d709716e89c85b9f8533caa`; the joined panel SHA-256 is
`f39e1d79cd4f8f8c5ae4ff16105946da382ddde20275b58c44e002084cda5d58`. It underperformed the
compact market panel: pooled AUC `0.507565` with sector ID retained and `0.504098` with sector ID
excluded. The rich branch is rejected.

The aggregator was hardened to deduplicate content hashes within each sector/session and to expose
unique-symbol coverage plus sentiment standard deviation, minimum, and maximum. The corrected
source already had no same-content duplicates within a sector/session, so the deduplication produced
the same panel bytes; the dispersion/coverage panel was nevertheless evaluated. Its pooled AUC was
`0.515669`, validation mean `0.520664`, test mean `0.518909`, and only `1/33` both-gate folds. It is
rejected. The panel SHA-256 is
`9f82c147ef6b0956524596963b55ea897cc25bd95d8cbb6b158a96191705b811`.

The final architecture check trained one classifier per ETF with source news coverage:

| ETF | Pooled test AUC | Validation mean | Test mean | Both gates |
|---|---:|---:|---:|---:|
| XLC | 0.467535 | 0.532162 | 0.482344 | 1/33 |
| XLE | 0.511839 | 0.544237 | 0.526548 | 2/33 |
| XLF | 0.471335 | 0.510940 | 0.485430 | 2/33 |
| XLI | 0.500398 | 0.538163 | 0.497627 | 4/33 |
| XLK | 0.544877 | 0.550409 | 0.553033 | 7/33 |
| XLP | 0.537436 | 0.545794 | 0.536844 | 5/33 |
| XLV | 0.490026 | 0.545528 | 0.498447 | 8/33 |
| XLY | 0.447964 | 0.538115 | 0.489150 | 4/33 |

No ETF clears the every-fold gate. Rolling 24- and 36-month training windows also failed with
pooled AUC `0.502016` and `0.509698`; they are not used to select a final artifact. The corrected
recommendation surface remains Neutral until a model clears the complete out-of-sample gate.

## Canonical FNSPID PIT session and filing-availability audit (2026-08-12)

The FNSPID importer and XLK feature builder were corrected and rerun after the prior experiments
used calendar dates and same-day holdings snapshots. The importer now assigns each source timestamp
to the first NYSE session whose 09:20 ET cutoff contains it, maps the event to the prior market row,
and records a row-level provenance ledger. Membership is resolved from the latest SEC snapshot
strictly before the assigned session. A second run used the SEC `available_from` filing date rather
than the quarter-end report date; this is the preferred date-only PIT approximation.

The availability-date import retained 68,518 rows, produced 68,518 unique provenance identities,
and resolved 2,381 rows against the carried-forward final filing. The provenance ledger SHA-256 is
`bf45c2d197dc23447beca76b7c16bd8225fb553fc3ff7fe0a2778d1d01102ad2`, and the import manifest
SHA-256 is `20a4a799dc8a645197002b180eddbeeb944905a66607c6785dd0249632e6f0ec`.

The fixed 33-fold protocol uses 11 purged sessions, a trailing six-month validation window, and
test months 2021-04 through 2023-12. The exact-availability panel results are:

| Profile | Pooled test AUC | Validation mean / minimum | Test mean / minimum | Both gates |
|---|---:|---:|---:|---:|
| FinBERT news | 0.722443 | 0.755726 / 0.712226 | 0.741681 / 0.547619 | 32/33 |
| Market + FinBERT news | 0.721683 | 0.756833 / 0.684871 | 0.760401 / 0.525253 | 32/33 |

The combined panel SHA-256 is `9b6ff0e788aed69edc24ca8686d867026a399eca7c69db1b20d714ecec937be8`.
The news and combined report SHA-256 values are `b5be208ed5113ab5e178b78d590455263efe6525766e35191c813f3dc7cee918`
and `5350fbdfd0c07aaad030afe6edbb21ea886eaac56782b680c3ed490753d125a2`.

A fixed label-shuffle negative control produced pooled AUC `0.464645`, confirming that the
evaluator does not generate the development lift from random labels. That control is not evidence
of predictive edge. The apparent development signal is therefore retained as an interesting
source-specific diagnostic, but the candidate is rejected because the every-fold gate fails.

The untouched 2024-01 through 2026-07 market holdout, where the historical FNSPID news panel is
correctly absent rather than backfilled, produced pooled AUC `0.477365`, accuracy `0.570093`, and
log loss `0.684035`; its report SHA-256 is
`70ae285521d7c684d74a764edfafe4a48b650069722a1173cc9e31b3781ce428`. This confirms that the
development result does not establish a stable cross-period edge. No model artifact is promoted,
no Sharpe is claimed from this candidate, and the recommendation surface remains fail-closed
Neutral. The next productive step is to certify a licensed timestamped news archive covering the
later holdout before any further model tuning.

The forensic differential audit found that the prior `0.476045` result and the new `0.721683`
result use identical dates, labels, and market features. The news aggregates are shifted by one
session: the old row for date `t` contains the prior event-session aggregate, while the new row
contains news assigned to the next session and eligible through that session's 09:20 ET cutoff.
This is the intended decision-time alignment, but the discontinuity is large enough that it remains
an explicit data-risk finding rather than proof of edge.

Four deterministic session-block label shifts of 20, 40, 60, and 80 rows produced pooled AUCs
`0.492072`, `0.473473`, `0.501808`, and `0.464094`, respectively. These negative controls are
consistent with no learnable signal after labels are displaced and reinforce the decision to reject
the development candidate pending a licensed later-period news archive. They do not validate the
model's historical AUC.

## Walk-forward purge correction and fold-stratified audit (2026-08-12)

The monthly evaluator now purges a configured number of distinct session keys rather than a
configured number of CSV rows. This matters for pooled ETF panels where one session contains one
row per sector; the protocol JSON now records the purge unit explicitly. XLK-only results are
unchanged because they have one row per session.

The corrected exact-availability FNSPID rerun remains a rejected development result. Its combined
model has pooled AUC `0.721683`, validation AUC minimum `0.684871`, test AUC minimum `0.525253`,
and `32/33` folds passing both strict `>0.55` gates; the sole failing test month is 2022-01. The
news-only model has pooled AUC `0.722443`, validation minimum `0.712226`, test minimum `0.547619`,
and `32/33` passing folds; its sole failing test month is 2023-02. The combined report SHA-256 is
`5350fbdfd0c07aaad030afe6edbb21ea886eaac56782b680c3ed490753d125a2`, and the news report SHA-256
is `b5be208ed5113ab5e178b78d590455263efe6525766e35191c813f3dc7cee918`.

A fold-stratified audit of the combined out-of-sample predictions found that the pooled AUC is not
being created by between-month score-scale ordering: pooled AUC was `0.721683`, pair-weighted
within-month AUC was `0.765418`, fold-mean-only AUC was `0.532914`, and within-month percentile
ranking still produced pooled AUC `0.752792`. One hundred deterministic label permutations that
preserved each month's positive rate had mean AUC `0.521789`, standard deviation `0.018278`, and
95% interval `[0.493570, 0.559707]`; none reached the observed AUC. The historical FNSPID lift is
therefore a genuine within-period diagnostic, but it still fails the all-fold gate and does not
survive the later 2024-2026 check (`0.477365`). It is not promoted.

As a separate source check, the existing corrected Luckycat FinBERT panel was transformed with the
new C++20 `arrakis-shift-news-panel` utility so each row receives the next session's news and its
final row is zero-filled. The transformation was audited against the next-row reference with zero
article-count mismatches and zero sentiment mismatches. The 20,900-row, 11-sector shifted panel has
SHA-256 `32d0c8d99b04bc68a7c338b09af77c8016367b4cae9d516a2d3d5d4edfcd5625`. It remains weak:
pooled corrected-panel AUC `0.509771` with a true 11-session purge, validation minimum `0.483163`,
test minimum `0.382786`, and only `4/33` passing folds. XLK-only AUC was `0.488038`, with test
minimum `0.245370`. This branch is rejected without tuning.

The research recommendation surface remains fail-closed Neutral. No directional artifact is
promoted and no Sharpe or profitability claim is made. Further tuning of the historical FNSPID
labels would be data-snooping; the next legitimate promotion attempt still requires a licensed,
timestamp-verified archive covering a sealed post-July-2026 evaluation period.

## SEC primary-document later-period extension (2026-08-12)

The local SEC submissions metadata was extended into the later evaluation period. The C++ event
builder retained 6,555 point-in-time sector filing events through 2026-07-24, including 2,775
events after 2024-01-01 across all 11 sector ETFs. The later branch downloaded 2,288 8-K primary
HTML documents from SEC Archives, extracted visible filing text with Item 9.01 omitted, and ran
the frozen FinBERT pooled-embedding graph on the local accelerator. It produced 1,759 unique model
inputs, 2,288 occurrence rows, 589 news sessions, and zero missing embeddings.

The historical and later SEC embedding panels were merged by a C++20 streaming utility at the
2024-01-01 boundary. The resulting panel contains 21,725 pooled sector rows from 2018-09-13
through 2026-07-24, 4,659 point-in-time SEC articles, 768 embedding features, and later news
through 2026-07-23. The panel SHA-256 is
`454d2acd42a4bb1e81d5c675969a9792dd9ac8c596d944e4ef7e0d3ae5d862eb`.

With the unchanged XGBoost configuration, 75 rounds, seed 42, monthly walk-forward, and a true
11-session purge represented as 121 pooled rows:

| SEC panel | Folds | Pooled AUC | Decision |
|---|---:|---:|---|
| Development, 2021-04 through 2023-12 | 33 | 0.502832 | rejected |
| Later holdout, 2024-01 through 2026-07 | 31 | 0.502794 | rejected |

The market-only control was similar (`0.501524` development and `0.502481` later holdout), so
the SEC embedding branch did not provide measurable directional lift. No SEC artifact is promoted;
the recommendation surface remains fail-closed Neutral and no Sharpe or predictive-edge claim is
made. The event manifest SHA-256 is
`7cc9fb561bd06bd19e555255bd2f0b3f14346a26aafe11f9b3cf650efbfe1219`, the later article CSV SHA-256
is `500d638c3222e1117703ec288a99362ddeb3b858a64645e1dde9ba383ab72a1a`, and the holdout report
SHA-256 is `c11608a477db4612ff80ab60617b517e3fb6efb57ad6b5d5f32fa4e113fa96f8`.

This is a completed source audit, not a license or profitability claim. The next iteration must
use a materially different, timestamped information source or a predeclared target/model change;
re-tuning SEC embeddings against these failed folds would be data-snooping.

### SEC purge correction (2026-08-12)

The table above records the earlier 8-K-only branch and should not be read as the final all-forms
result: its command used `--purge-sessions 121`, which was later identified as 121 distinct
sessions rather than the intended 11 sessions (121 is the number of pooled rows in an 11-session
window). The corrected all-supported SEC branch uses `--purge-sessions 11` and contains 2,775
later-period events across all supported SEC forms. It remains rejected:

| SEC all-supported panel | Folds | Pooled AUC | Min validation AUC | Min test AUC |
|---|---:|---:|---:|---:|
| Development, 2021-04 through 2023-12 | 33 | 0.516837 | 0.452085 | 0.332827 |
| Later holdout, 2024-01 through 2026-07 | 31 | 0.486694 | 0.470999 | 0.376724 |

The pooled panel SHA-256 is `8a7849702aaa70b33841c0e8af69405725a3317670a464aaa47fff68593c07b8`;
the corrected report hashes are `46ffb55339851f4c0c56ae3ea2e35796a5c689586bbd9ce99d9acda94994d9d1`
and `0846714c5587011e76c9e39546e883b8d693506cb933fbf900104a6153838a01`. Per-ETF classifiers
also failed to clear the gate: the development/holdout AUCs were XLB `0.512925/0.496062`, XLC
`0.475696/0.475911`, XLE `0.509540/0.532353`, XLF `0.422518/0.485737`, XLI `0.504507/0.486753`,
XLK `0.482438/0.508593`, XLP `0.526949/0.462092`, XLRE `0.491271/0.495927`, XLU
`0.499808/0.514846`, XLV `0.507380/0.494711`, and XLY `0.471446/0.508882`.

## Opening-gap nowcaster and intraday control (2026-08-12)

The next predeclared experiment used the local daily OHLC history to build an explicitly separate
post-open diagnostic. The new C++20 `open-gap-direction` mode shifts all rolling and market features
to session `t-1`, then adds the sector, SPY, and QQQ opening gaps observed for session `t`. Its label
is `close[t] > close[t-1]`. This is a 09:30-style current-close nowcast, not the canonical
next-session-close recommendation target, and the daily `open` fields are only a proxy for an
executable opening print.

With 75 rounds, seed 42, six-month trailing validation, monthly outer folds, and an 11-session
purge, the pooled XGBoost result passed every available fold in both periods:

| Dataset | Folds | Pooled AUC | Min validation AUC | Min test AUC | Passing folds |
|---|---:|---:|---:|---:|---:|
| Development, 2021-04 through 2023-12 | 33 | 0.730744 | 0.699958 | 0.553114 | 33/33 |
| Untouched holdout, 2024-01 through 2026-07 | 31 | 0.736851 | 0.693653 | 0.644131 | 31/31 |

A five-column gap-only control reproduced the result (`0.732294` development and `0.737717`
holdout), while the raw sector-gap sign heuristic had full-panel accuracy `0.681657` and AUC
`0.746715`. Therefore the apparent performance is almost entirely the observed overnight gap,
not FinBERT or XGBoost complexity. The dataset SHA-256 is
`030b6b3ba4b8e6e093f0f70cae85a4a6bf65540cc6f795b0e9eeee672949e3e7`; the development and holdout
report hashes are `cadd3a9b43af64f3defe71fa1decb60f8263fac7d0772492eeb8b6fd5a25b5fd` and
`dd870ed9b84ac2300d79a59b658db708b298c15a6ff789f8bcaaf811b2374fc3`.

To test whether the gap predicts future post-open continuation rather than merely classifying the
already-observed overnight move, `intraday-direction` kept the same pre-open features and changed
the label to `close[t] > open[t]`. It failed: development AUC `0.483475` and holdout AUC
`0.488842`, with only `2/33` and `0/31` folds passing the strict per-fold gate. This validates the
nowcaster decomposition but does not establish an actionable intraday edge. Its report hashes are
`52a53cc7c0d362ba4e97e5bbf92fd40f9c0bfa3dc878381480bdf0aa3d2701e3` and
`b637e90abc3ab4ac26a218d6064ce9bf4159cb0329115a9f8dcd03a1701039d7`; the intraday dataset
SHA-256 is `c72459b92726ae9a9b360c3fe52ce11aea31fdb04ffecbc33b6a1008263c9e6c`.

The nowcaster is not promoted to the canonical recommendation surface. A real promotion would
require point-in-time exchange prints after the opening auction, an executable post-open quote or
VWAP target, and a cost-aware backtest. The canonical next-session-close model remains rejected and
the production research surface remains fail-closed Neutral.

## Canonical market-only follow-up (2026-08-12)

After rejecting the opening-gap nowcast, the canonical target was re-evaluated on the complete
2018-09-13 through 2026-07-24 11-ETF market panel without news. The same 75-round XGBoost and
11-session purged monthly protocol produced pooled AUC `0.500865` on the 33-month development
period and `0.499979` on the untouched 31-month later holdout. The panel SHA-256 is
`0e386926295572430a022d05cc751dcd6bfe8f485c530ad7388ac7ce4afe9038`; report hashes are
`e0a7b963d2db885645b7455a4fb3fcefabb3aa8a61198003cea064c8f03ef261` and
`01c7658c063e304827aaf07974f4e3fc1716e1c0d57226f3268fb69fc945d93c`.

The required per-ETF market-only architecture also failed to produce a promotable sector. The
best pooled development/holdout pairs were XLV `0.520812/0.545119`, XLP `0.531938/0.516730`, and
XLU `0.518564/0.522978`; none clears both strict per-fold gates. A cross-sectional `rank:pairwise`
XGBoost control over the same sessions produced 79 folds and pooled AUC `0.499360`, with report
SHA-256 `851873410dbad4db5f6a0178355e8620a58213f5300c6a8b82ca868996c6a389`.

Two predeclared feature extensions were also rejected. Long-horizon trend features (24/60/120/252
sessions and long SMAs) produced development/holdout AUC `0.506612/0.498485`, with only `3/33`
and `2/31` folds passing; dataset and report hashes are
`b681f35589307214fa817d9c28fb9f1627d3191b1eb0be05861b6129a2eb5856`,
`53f5ec09cf165681491d0aafa0c80787ceb6ef138e488761f3e7490db44abf2f`, and
`cf774c5fc0d298ef6f393e215b64f3dab435d243780608b028453e13d7ab500e`. Existing VIX level/change
features produced AUC `0.513780/0.500520`, with `0/33` and `1/31` folds passing; hashes are
`5f4e39fb6976a743645b2fb27ea1a45ade309a77490352eca0c783867b8b8e51`,
`5319350a3fe63c35934278236e6f05da57bb81f610f00779894e14ff6045b211`, and
`0fd7a91b2bf2a866e0f4a2ce15462d3c7514b140cf7d3008be41545932a68521`.

These controls exhaust the useful local daily-feature variants without touching the later holdout.
The next legitimate promotion attempt requires a frozen 09:20 ET timestamped extended-hours
dataset; daily OHLC cannot reconstruct that snapshot without leakage. Until then, the canonical
recommendation gate remains closed and the surface remains fail-closed Neutral.

An additional open-time target, `close[t+1] > open[t]`, was tested with the same opening-gap
features as a bounded alternative. It produced pooled AUC `0.546480` on development and `0.488272`
on the later holdout, with only `7/33` and `1/31` folds passing. The dataset and report hashes are
`ea2bac1591349582e2e5470fd6f8c897bac34e6489ed73009b5709ce8bd382bb`,
`b9a7aae4b65689248bcb936178265c122d13da99a483352c5d1b3133ddc33c1a`, and
`f78154cbed0aada2d3bdf375f17c2edc26e7933aa5ac9b004665bf2b6abf87ee`. It is rejected as well.

The local search is now frozen. Further daily-feature or hyperparameter iterations against the
same inspected holdout would be multiple-testing rather than research progress. Resumption requires
all supported ETF, SPY, and QQQ timestamped extended-hours observations (including event timestamps,
staleness/missingness, corrections, and corporate-action metadata) through a sealed cutoff. Data
after July 2026 must remain reserved for prospective confirmation.

For completeness, the rich close-time market schema was rebuilt through the same later holdout. It
produced development/holdout pooled AUC `0.510467/0.487606`, with `5/33` and `0/31` folds passing;
the dataset SHA-256 is `6242542aff8c3a86f15a65a92b47b7a7191659b62cd5d74daff56d7c1a218e9e`, and the
report hashes are `85d0fbc25dbef24f1e5ee5bf3db48adf1fedeab71fd8fdd3d1047cc9cfef1060` and
`452a7c8bc47442994fc3cd699aac552556265a18447cac49ba593cf679efe9a9`.
