# Legacy market-bars-only ML path

`services/feature_engine/` and `services/prediction_service/` are retained as an
archived, tested market-bars-only vertical slice. They are not part of the
default Docker runtime, are not consumed by the frontend, and must not be
described as part of the active FinBERT-plus-XGBoost news recommendation path.

They may be revisited in a future research phase for a controlled price-only
benchmark or Kafka replay comparison. Any such work must use a separate model
registry namespace and evaluation artifacts; it must not silently replace or
feed the production research-only news path. The final news system does not
train, retain, or serve Logistic Regression artifacts.
