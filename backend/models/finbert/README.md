# FinBERT artifact contract

Place the frozen, exported FinBERT ONNX model at `model.onnx` and its exact WordPiece vocabulary at `vocab.txt` before starting the news profile. The runtime validates both files and fails closed when either artifact is missing.

The exported graph must accept `input_ids`, `attention_mask`, and optionally `token_type_ids`. Output 0 must be three-class logits in the FinBERT order positive, negative, neutral. Output 1 should be a `[batch, embedding_dimensions]` pooled embedding.
