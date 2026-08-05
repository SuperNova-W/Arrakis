# FinBERT artifact contract

Place the frozen, exported FinBERT ONNX model at `model.onnx` and its exact WordPiece vocabulary at `vocab.txt` before starting the news profile. The runtime validates both files and fails closed when either artifact is missing.

The exact Arrakis export is deliberately not committed because the ONNX file is larger than GitHub's normal file limit. Publish that export and its vocabulary to versioned operator-controlled HTTPS or S3-compatible object-storage URLs, then fetch them with checksum verification:

```sh
ARRAKIS_FINBERT_MODEL_URL=https://object-storage.example/arrakis/finbert-v1/model.onnx \
ARRAKIS_FINBERT_VOCAB_URL=https://object-storage.example/arrakis/finbert-v1/vocab.txt \
./scripts/fetch_finbert_model.sh
```

The expected SHA-256 values and tensor contract are checked in `manifest.json`. Override the checksum variables only when intentionally publishing a new version and update the manifest in the same change. Public FinBERT exports are not interchangeable: the runtime requires the logits order and optional pooled-embedding output described below.

The exported graph must accept `input_ids`, `attention_mask`, and optionally `token_type_ids`. Output 0 must be three-class logits in the FinBERT order positive, negative, neutral. Output 1 should be a `[batch, embedding_dimensions]` pooled embedding.
