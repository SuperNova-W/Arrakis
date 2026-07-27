import csv
import json
import math
import os
from pathlib import Path

import numpy as np
import pandas as pd
from xgboost import XGBRegressor
from sklearn.metrics import mean_squared_error, r2_score

ROOT = Path(__file__).resolve().parent
DATA_DIR = ROOT / 'data' / 'history'
OUT_DIR = ROOT / 'artifacts' / 'sector_xgb'
OUT_DIR.mkdir(parents=True, exist_ok=True)

TARGET = 'XLK'
CONTEXT = ['SPY', 'QQQ', 'IWM', 'TLT', 'HYG', 'GLD', 'USO']


def load_symbol(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    df['timestamp_utc'] = pd.to_datetime(df['timestamp_utc'], unit='s', utc=True)
    df = df.sort_values('timestamp_utc')
    return df


def build_dataset() -> tuple[pd.DataFrame, list[str]]:
    frames = {symbol: load_symbol(DATA_DIR / f'{symbol}.csv') for symbol in [TARGET] + CONTEXT}
    target = frames[TARGET].rename(columns={'close': 'target_close'})
    target = target[['timestamp_utc', 'target_close', 'volume']].copy()
    target = target.rename(columns={'volume': 'target_volume'})

    for symbol in CONTEXT:
        ctx = frames[symbol].rename(columns={'close': f'{symbol.lower()}_close', 'volume': f'{symbol.lower()}_volume'})
        target = target.merge(ctx[['timestamp_utc', f'{symbol.lower()}_close', f'{symbol.lower()}_volume']], on='timestamp_utc', how='inner')

    target['ret_1'] = target['target_close'].pct_change(1)
    target['ret_3'] = target['target_close'].pct_change(3)
    target['ret_6'] = target['target_close'].pct_change(6)
    target['volatility_6'] = target['ret_6'].abs().rolling(6).mean()
    target['volume_mean_6'] = target['target_volume'].rolling(6).mean()
    target['rel_volume'] = target['target_volume'] / target['volume_mean_6'].replace(0, np.nan)
    target['rsi_14'] = 100 - (100 / (1 + target['ret_1'].rolling(14).mean().abs() + 1e-8))

    target['future_return'] = target['target_close'].pct_change(-6).shift(-6)
    target['target_label'] = (target['future_return'] > 0).astype(int)

    target = target.dropna().reset_index(drop=True)
    target['date'] = target['timestamp_utc'].dt.strftime('%Y-%m-%d')
    return target, [
        'ret_1', 'ret_3', 'ret_6', 'volatility_6', 'volume_mean_6', 'rel_volume', 'rsi_14',
        'spy_close', 'qqq_close', 'iwm_close', 'tlt_close', 'hyg_close', 'gld_close', 'uso_close'
    ]


def main():
    df, feature_names = build_dataset()
    split_idx = int(len(df) * 0.8)
    train_df = df.iloc[:split_idx]
    test_df = df.iloc[split_idx:]

    X_train = train_df[feature_names].astype(float).to_numpy()
    y_train = train_df['target_label'].astype(int).to_numpy()
    X_test = test_df[feature_names].astype(float).to_numpy()
    y_test = test_df['target_label'].astype(int).to_numpy()

    model = XGBRegressor(
        n_estimators=200,
        learning_rate=0.05,
        max_depth=4,
        subsample=0.9,
        colsample_bytree=0.8,
        objective='reg:squarederror',
        eval_metric='rmse',
        tree_method='hist',
        n_jobs=4,
        random_state=1337,
    )

    model.fit(X_train, y_train)
    preds = model.predict(X_test)
    rmse = mean_squared_error(y_test, preds) ** 0.5
    r2 = r2_score(y_test, preds)

    out_path = OUT_DIR / 'xlk_sector_xgb.json'
    with out_path.open('w', encoding='utf-8') as fh:
        json.dump({
            'target': TARGET,
            'rows': int(len(df)),
            'train_rows': int(len(train_df)),
            'test_rows': int(len(test_df)),
            'features': feature_names,
            'rmse': float(rmse),
            'r2': float(r2),
            'test_accuracy': float((np.round(preds) == y_test).mean()),
            'test_positive_rate': float(y_test.mean()),
            'sample_predictions': [float(x) for x in preds[:10]],
        }, fh, indent=2)

    print(json.dumps({
        'target': TARGET,
        'rows': int(len(df)),
        'train_rows': int(len(train_df)),
        'test_rows': int(len(test_df)),
        'rmse': float(rmse),
        'r2': float(r2),
        'test_accuracy': float((np.round(preds) == y_test).mean()),
        'test_positive_rate': float(y_test.mean()),
        'model_path': str(out_path),
    }, indent=2))


if __name__ == '__main__':
    main()
