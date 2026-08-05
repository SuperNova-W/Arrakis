# XLK target sanity check and target evaluation

Date: 2026-08-05

## Protocol

The runs used `backend/data/fnspid/normalized/xlk_combined_features.csv` (1,251 rows,
36 combined market/news features), with the same chronological boundaries for every target:

- Train: 2019-01-10 through 2021-12-31, 751 rows
- Validation: 2022-01-03 through 2022-12-30, 251 rows
- Test: 2023-01-03 through 2023-12-28, 249 rows

XGBoost used the default configuration: `eta=0.05`, `max_depth=3`,
`min_child_weight=1`, `subsample=0.8`, `colsample_bytree=0.8`, `lambda=1`,
`alpha=0`, seed 42, 75 maximum rounds, and 20 validation rounds without improvement.
No test-set tuning or hyperparameter search was used. The derived targets use XLK history;
excess-return targets additionally use SPY history.

The new `target_high_volatility_next_day` label is 1 when the absolute next-day XLK return
is strictly above the median of the 20 realized absolute daily returns ending on the current
prediction date. The next-day return is therefore not used in its own threshold.

## 1. Constant-prediction sanity check

For the combined-feature default `target_next_close_up` run, test probabilities were:

| Statistic | Value |
|---|---:|
| Minimum | 0.524371 |
| Maximum | 0.571693 |
| Mean | 0.562721 |
| Std. dev. | 0.009109 |
| Distinct probabilities | 19 |

The ten-bin histogram, for bins `[0.0,0.1)`, ..., `[0.9,1.0]`, was:

```text
[0, 0, 0, 0, 0, 249, 0, 0, 0, 0]
```

At threshold 0.5 the confusion matrix was `TN=0, FP=105, FN=0, TP=144`.
The test labels contained 144 positives and 105 negatives, so the directly computed
majority-class baseline was label 1 with accuracy `144/249 = 0.578313`. The model also
had accuracy `0.578313`.

Verdict: **(b), a genuine null result with practical near-constant probabilities, not a
literal constant model.** The model makes small but real probability changes (19 values,
standard deviation 0.009109), but every value remains in the same 0.5–0.6 bucket and every
thresholded prediction is positive. The test ROC AUC of 0.495370 confirms that the small
ranking variation is not useful. The market-only, news-only, and combined runs likewise
predicted every test row positive, with test AUCs 0.5141, 0.4709, and 0.4954 respectively;
their threshold accuracy equality is therefore mostly a majority-baseline artifact, while
the AUC differences still provide an informative ablation signal.

The mechanism is not an early-stopping implementation bug or a regularizer that prevents
trees from learning. With the default combined run, validation log loss was 0.712637 at
iteration 0, reached its best value of 0.712208 at iteration 1 (the reported selected
iteration is 2 using one-based numbering), then rose to 0.717592 at iteration 9 and
0.725367 at iteration 19. With early stopping disabled, training continued to round 75:
training log loss fell to 0.558141 while validation log loss worsened to 0.755332, and
checkpoint restoration still selected iteration 2. The model is learning the training
sample, whose positive rate is 0.563249, but the 2022 validation positive rate is only
0.454183. The evidence is consistent with a weak/no stable out-of-sample relationship and
validation regime/base-rate shift, not with the model being unable to fit any feature split.

## 2. Target comparison

Each cell is `accuracy / log loss / ROC AUC`. Because the fixed protocol has one calendar year
in each out-of-sample partition, the 2022 validation and 2023 test columns are also the
per-year breakdowns. The generated JSON artifacts retain the explicit `validation_by_year`
and `test_by_year` arrays for auditability.

| Target | Validation: 2022 | Test: 2023 | Test majority accuracy |
|---|---:|---:|---:|
| `target_next_close_up` | 0.4542 / 0.7122 / 0.5288 | 0.5783 / 0.6812 / 0.4954 | 0.5783 |
| `forward_return_3d_up` | 0.4382 / 0.7539 / 0.4892 | 0.5783 / 0.6798 / 0.6030 | 0.5783 |
| `forward_return_5d_up` | 0.4582 / 0.7661 / 0.4781 | 0.6185 / 0.6631 / 0.5576 | 0.6185 |
| `excess_return_3d_up` | 0.4462 / 0.7323 / 0.5473 | 0.5944 / 0.6653 / 0.5918 | 0.5984 |
| `excess_return_5d_up` | 0.4422 / 0.7440 / 0.5187 | 0.5984 / 0.6725 / 0.5011 | 0.6225 |
| `target_high_volatility_next_day` | 0.5418 / 0.6896 / 0.5673 | 0.5221 / 0.6910 / 0.5446 | 0.5181 |

No target clears the pre-registered “promising” bar of validation AUC > 0.55 and test AUC
> 0.55. The volatility target is the closest: validation AUC is 0.5673, but test AUC falls
to 0.5446. The 3-day and excess-3-day targets have attractive test AUCs, but their validation
AUCs are below 0.55. Their isolated test improvements should therefore be treated as noise
until reproduced on additional untouched walk-forward periods.

## Recommendation

Do not promote any of these targets or feature subsets to a production research recommendation
path yet. The added volatility target is a valid, leakage-safe experiment and is worth retaining
as a candidate for broader walk-forward evaluation, but this single fixed holdout does not show
validation-confirmed predictive signal. Move effort toward expanding the number of untouched
walk-forward periods, improving point-in-time news coverage and sample size, and testing whether
the volatility target remains useful across regimes before adding model complexity.

The complete generated model, manifest, metrics, and held-out prediction artifacts are in the
ignored directory:

`backend/artifacts/xlk_target_evaluation_2026-08-05/`

