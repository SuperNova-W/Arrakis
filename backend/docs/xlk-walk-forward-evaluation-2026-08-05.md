# XLK walk-forward target evaluation

Date: 2026-08-05

## Protocol

This run extends the fixed-split evaluation in
`xlk-target-evaluation-2026-08-05.md` without changing the 36-feature schema or the
six target definitions. It used:

- Dataset: `backend/data/fnspid/normalized/xlk_combined_features.csv`
- Rows: 1,251 daily XLK feature vectors from 2019-01-10 through 2023-12-28
- Features: the existing combined 9 market + 27 news features
- Model: XGBoost binary logistic, CPU histogram method
- Parameters: `eta=0.05`, `max_depth=3`, `min_child_weight=1`, `subsample=0.8`,
  `colsample_bytree=0.8`, `lambda=1`, `alpha=0`, seed 42
- Training: 75 maximum rounds with 20-round validation-log-loss early stopping
- Hyperparameter search: disabled; no ensembling or architecture changes

The available history yields three expanding walk-forward windows. Test years do not
overlap. For each window, the model is retrained from scratch, the validation year is
used only for early stopping, and the test year is evaluated once after model selection.

| Window | Train | Validation | Test |
|---|---|---|---|
| 2021 test | 2019-01-10–2019-12-31 (246) | 2020 (253) | 2021 (252) |
| 2022 test | 2019-01-10–2020-12-31 (499) | 2021 (252) | 2022 (251) |
| 2023 test | 2019-01-10–2021-12-31 (751) | 2022 (251) | 2023 (249) |

The result artifact is `backend/artifacts/xlk_walk_forward_2026-08-05.json`. Each
metric cell below is `accuracy / log loss / ROC AUC`.

## Per-window results

| Target | 2021 validation | 2021 test | 2022 validation | 2022 test | 2023 validation | 2023 test |
|---|---:|---:|---:|---:|---:|---:|
| `target_next_close_up` | 0.5771 / 0.6796 / 0.5475 | 0.5119 / 0.6988 / 0.5717 | 0.5198 / 0.6951 / 0.5717 | 0.4462 / 0.7298 / 0.5069 | 0.4542 / 0.7122 / 0.5288 | 0.5783 / 0.6812 / 0.4954 |
| `forward_return_3d_up` | 0.6245 / 0.6597 / 0.5426 | 0.6032 / 0.6726 / 0.5588 | 0.6111 / 0.6700 / 0.4773 | 0.4382 / 0.7586 / 0.4829 | 0.4382 / 0.7539 / 0.4892 | 0.5783 / 0.6798 / 0.6030 |
| `forward_return_5d_up` | 0.6403 / 0.6609 / 0.5310 | 0.6190 / 0.6793 / 0.5329 | 0.6190 / 0.6718 / 0.4714 | 0.4582 / 0.7800 / 0.5058 | 0.4582 / 0.7661 / 0.4781 | 0.6185 / 0.6631 / 0.5576 |
| `excess_return_3d_up` | 0.5889 / 0.6795 / 0.5559 | 0.5714 / 0.6884 / 0.5489 | 0.5675 / 0.6829 / 0.5678 | 0.4183 / 0.7497 / 0.5129 | 0.4462 / 0.7323 / 0.5473 | 0.5944 / 0.6653 / 0.5918 |
| `excess_return_5d_up` | 0.5889 / 0.6806 / 0.4854 | 0.5913 / 0.6782 / 0.5580 | 0.5913 / 0.6761 / 0.5145 | 0.4223 / 0.7522 / 0.4824 | 0.4422 / 0.7440 / 0.5187 | 0.5984 / 0.6725 / 0.5011 |
| `target_high_volatility_next_day` | 0.5810 / 0.6799 / 0.6149 | 0.5794 / 0.6872 / 0.5775 | 0.5873 / 0.6728 / 0.6237 | 0.5339 / 0.7202 / 0.5610 | 0.5418 / 0.6896 / 0.5673 | 0.5221 / 0.6910 / 0.5446 |

## Promotion decision

No target is a promotion candidate. For this harness, “stable” means clearing the
pre-registered validation AUC > 0.55 and test AUC > 0.55 bar in every available
walk-forward window. This prevents a single favorable regime from being treated as a
validated recommendation target.

`target_high_volatility_next_day` is the strongest result: it clears both AUC thresholds
in the 2021 and 2022 windows, but its 2023 test AUC is 0.5446. It therefore remains an
unpromoted candidate, not evidence of a production research signal. The other targets
either miss the validation bar, the test bar, or both in multiple windows. The attractive
2023 test AUC of 0.6030 for `forward_return_3d_up` is not validation-confirmed and is not
stable across the earlier windows.

These results support the existing rule: do not add hyperparameter search, ensembling, or
new model architectures until a target clears the bar across multiple untouched periods.
The next evidence-producing steps are broader point-in-time coverage, more history, and
additional untouched periods when the data supports them.
