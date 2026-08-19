# XLK TCN GPU challenger

This document records the experimental TCN path for the XLK combined-feature dataset. It is a
research challenger only. The active recommendation gate remains the FinBERT plus XGBoost path and
is unchanged by this experiment.

## Runtime

The trainer is C++20 and uses the Apple MLX C++ API with the Metal GPU backend. It is intentionally
GPU-only: the CLI accepts `--device gpu` and rejects CPU execution. The local verification machine
reported:

```text
MLX device=Apple M5 backend=gpu
```

Install MLX C++ once on an Apple Silicon development machine:

```bash
brew install mlx-c
```

Configure and build the opt-in target from `backend/`:

```bash
cmake -S . -B build/tcn -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DARRAKIS_BUILD_TCN_TRAINER=ON \
  -DARRAKIS_BUILD_MARKET_STREAM=OFF \
  -DARRAKIS_BUILD_MARKET_API=OFF \
  -DARRAKIS_BUILD_ML_RUNTIME=OFF
cmake --build build/tcn --target arrakis-train-tcn -j 4
```

Run the fixed chronological holdout:

```bash
./build/tcn/arrakis-train-tcn \
  --epochs 300 --patience 35 \
  --output-dir artifacts/tcn_xlk_gpu_w30_c16
```

Run the three documented expanding folds:

```bash
./build/tcn/arrakis-train-tcn \
  --walk-forward --epochs 300 --patience 35 \
  --output-dir artifacts/tcn_xlk_gpu_w30_c16_walk_forward
```

The second iteration uses the compact profile and a smaller normalized network:

```bash
./build/tcn/arrakis-train-tcn \
  --feature-profile compact --window 16 --hidden-channels 8 \
  --learning-rate 0.0003 --weight-decay 0.01 \
  --gradient-clip 1.0 --warmup-epochs 5 \
  --walk-forward --epochs 300 --patience 20 \
  --output-dir artifacts/tcn_xlk_gpu_v3_norm_compact_w16_c8_walk_forward
```

## Model and leakage controls

The model is a residual causal temporal convolutional network:

- 36 standardized XLK features per timestep;
- a kernel-1 input projection to 16 channels;
- four residual convolution blocks with kernel size 3 and dilations 1, 2, 4, and 8;
- ReLU activations and a final last-timestep binary classification head;
- standardization statistics fit only on the training portion of each fold;
- each validation and test sequence uses only history available through its prediction row.

The dataset contains 1,251 ordered rows from 2019-01-10 through 2023-12-28, with 36 named
price/news/embedding features and `target_next_close_up` as the label.

The high-reasoning review found that the original 36-column input contains zero-valued embedding
columns and several duplicate or effectively duplicate news columns. The revised trainer therefore
supports an 18-column compact profile, applies `log1p` to count/volume inputs, caps freshness, starts
variance accumulation at zero, and purges each split's final feature row because its one-day target is
realized in the next partition. It also adds AdamW-style weight decay, gradient clipping, warmup,
cosine decay, and channel normalization. These changes remain experimental and do not modify the
active FinBERT plus XGBoost recommendation path.

## Recorded results

All values below are out-of-sample test results from the three chronological folds. The mean columns
are simple means across folds, not pooled predictions.

| Configuration | Mean accuracy | Mean log loss | Mean ROC AUC |
| --- | ---: | ---: | ---: |
| window 30, channels 16 | 0.527831 | 0.870941 | 0.537971 |
| window 20, channels 8 | 0.511905 | 0.934783 | 0.505750 |
| window 30, channels 32 | 0.505365 | 0.810441 | 0.509640 |

The corrected compact-profile iteration (window 16, channels 8, normalized, purged folds) produced
mean test accuracy `0.480021`, mean log loss `0.854727`, and mean ROC AUC `0.504757` across the same
three calendar folds. Its fixed-holdout accuracy was `0.546185`, but its fixed-holdout ROC AUC was
only `0.513955`; the fixed-holdout gain did not survive walk-forward testing. A validation-selected
classification threshold of `0.81` reduced the fixed-holdout accuracy to `0.510040`, so threshold
selection is not treated as an improvement.

The second high-reasoning review identified two malformed market-feature proxies in the upstream
feature generator: `volatility_6` was derived from the magnitude of a single six-day return, and
`rsi_14` was a clipped transform of `ret_1`. They were replaced with sample standard deviation over
the six one-day log returns and a fourteen-change RSI, respectively. The combined feature schema was
bumped to v2, a deterministic C++ repair utility was added, and golden tests cover both calculations.
The corrected dataset was then retrained with the same compact TCN configuration on the GPU. This
paired candidate produced mean accuracy `0.505307` at validation-selected thresholds, mean log loss
`0.891972`, and mean ROC AUC `0.494444`; per-fold ROC AUC was `0.499527`, `0.483673`, and `0.500132`
for 2021, 2022, and 2023. The accuracy is not directly comparable with the earlier compact run,
which used the fixed `0.5` threshold; the threshold-independent AUC is the relevant comparison.
Because the corrected feature candidate reduced walk-forward AUC, it is recorded as a negative
ablation and is not promoted.

The next high-reasoning review recommended removing hidden channel normalization and fitting a
positive-slope Platt calibrator on each validation fold. Both options are now implemented in the
C++ trainer. The calibrator is fit with MLX on the GPU and is applied only after model selection; the
test period remains untouched. A five-seed sweep (41 through 45) on the corrected dataset produced:

| Seed | Mean accuracy | Mean calibrated log loss | Mean ROC AUC |
| ---: | ---: | ---: | ---: |
| 41 | 0.518518 | 0.703067 | 0.524928 |
| 42 | 0.463883 | 0.710590 | 0.524585 |
| 43 | 0.467899 | 0.713126 | 0.487227 |
| 44 | 0.481174 | 0.701965 | 0.515279 |
| 45 | 0.479840 | 0.705251 | 0.495082 |

Across seed means, the average accuracy was `0.482263`, average calibrated log loss was `0.706800`,
and average ROC AUC was `0.509420`; the median seed mean AUC was `0.515279`. The positive-slope
calibration preserves ranking, and the reconstructed raw-versus-calibrated check showed lower log
loss on all 15 held-out fold outputs. This improves probability calibration, but it does not improve
the original window-30/channels-16 TCN's mean AUC of `0.537971` or its mean accuracy of `0.527831`.
The v6 result therefore remains an experimental calibrated challenger and is not promoted to the
recommendation path.

The selected TCN configuration for continued research is window 30 / channels 16 because it had the
best mean ROC AUC among the tested TCN variants. Its per-fold ROC AUC was 0.591225, 0.491868, and
0.530820 for 2021, 2022, and 2023 respectively, which is regime-unstable and not sufficient for
promotion.

On the same fixed holdout, the existing XGBoost artifact reports accuracy 0.534137, log loss
0.750159, and ROC AUC 0.457937. The TCN fixed-holdout result was accuracy 0.518072, log loss
0.763687, and ROC AUC 0.520304. This is not a model promotion claim: the walk-forward evidence is
still mixed and the TCN has not been connected to the active recommendation service.

## Artifacts

The selected fixed-holdout run writes:

- `artifacts/tcn_xlk_gpu_w30_c16/weights.safetensors`
- `artifacts/tcn_xlk_gpu_w30_c16/manifest.json`
- `artifacts/tcn_xlk_gpu_w30_c16/metrics.json`
- `artifacts/tcn_xlk_gpu_w30_c16/test_predictions.csv`
- `artifacts/tcn_xlk_gpu_v3_norm_compact_w16_c8/metrics.json`

The manifest records the dataset SHA-256, feature order, normalization statistics, fold boundaries,
seed, architecture, and detected GPU device. Walk-forward artifacts are under
`artifacts/tcn_xlk_gpu_w30_c16_walk_forward/` and
`artifacts/tcn_xlk_gpu_v3_norm_compact_w16_c8_walk_forward/`. The corrected-market ablation is under
`artifacts/tcn_xlk_gpu_v5_corrected_market_w16_c8/`, with source data at
`data/fnspid/normalized/xlk_combined_features_market_v2.csv`. The five v6 calibrated seed runs are
under `artifacts/tcn_xlk_gpu_v6_rank_cal_seed41/` through
`artifacts/tcn_xlk_gpu_v6_rank_cal_seed45/`.

No transaction-cost-aware TCN strategy result has been claimed yet. The next research step is to
add the TCN predictions to the existing C++ event-driven backtester under the same threshold,
slippage, spread, turnover, and exposure assumptions used for the XGBoost comparison.
